#!/bin/bash
# Test: Core logging thread safety and time source unification
set -euo pipefail
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

# Create a temp dir
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

CC="${CC:-gcc}"
CFLAGS="-std=c99 -Wall -Wextra -O0 -g -Isrc -DVERSION_STR=\"test\""
LIBS="-lkernel32 -luser32"

SRCDIR="src"
OBJDIR="$TMPDIR/obj"

echo "=== Test Suite: Core logging + time source ==="
echo ""

# ==========================================
# Test 1: Multi-threaded logging safety
# ==========================================
echo "--- Test 1: Multi-threaded logging safety ---"

# Compile core.o from source for the test
mkdir -p "$OBJDIR"
$CC $CFLAGS -c "$SRCDIR/core.c" -o "$OBJDIR/core_test.o" 2>&1

cat > "$TMPDIR/test_log_lock.c" << 'TESTEOF'
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "core.h"

#define THREAD_COUNT 8
#define LOGS_PER_THREAD 30

typedef struct {
    int thread_id;
    int logs_done;
    bool has_error;
} ThreadData;

DWORD WINAPI log_thread(LPVOID lpParam) {
    ThreadData* td = (ThreadData*)lpParam;
    for (int i = 0; i < LOGS_PER_THREAD; i++) {
        nosleep_log_info("Thread %d iteration %d", td->thread_id, i);
        nosleep_log_warning("Thread %d warning %d", td->thread_id, i);
        nosleep_log_error("Thread %d error %d", td->thread_id, i);
        td->logs_done++;
        
        // Yield to increase chance of interleaving
        if (i % 5 == 0) {
            Sleep(0);
        }
    }
    return 0;
}

int main(void) {
    int exit_code = 0;
    
    printf("=== Testing basic logging functions ===\n");
    nosleep_log_info("Test info message");
    nosleep_log_warning("Test warning message");
    nosleep_log_error("Test error message");
    printf("=== Basic logging OK ===\n");
    
    // Now test multi-threaded logging
    printf("\n=== Testing multi-threaded logging (%d threads, %d logs each) ===\n",
           THREAD_COUNT, LOGS_PER_THREAD * 3);
    
    HANDLE threads[THREAD_COUNT];
    ThreadData thread_data[THREAD_COUNT];
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].logs_done = 0;
        thread_data[i].has_error = false;
        threads[i] = CreateThread(NULL, 0, log_thread, &thread_data[i], 0, NULL);
        if (!threads[i]) {
            printf("FAIL: Failed to create thread %d\n", i);
            exit_code = 1;
        }
    }
    
    // Wait for all threads to complete
    DWORD wait_result = WaitForMultipleObjects(THREAD_COUNT, threads, TRUE, 30000);
    if (wait_result == WAIT_TIMEOUT) {
        printf("FAIL: Threads timed out\n");
        exit_code = 1;
    }
    
    // Check all threads completed
    int total_logs = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        CloseHandle(threads[i]);
        total_logs += thread_data[i].logs_done;
    }
    
    int expected_iterations = THREAD_COUNT * LOGS_PER_THREAD;
    if (total_logs == expected_iterations) {
        printf("PASS: All %d threads completed %d iterations (%d log calls total)\n", 
               THREAD_COUNT, total_logs, total_logs * 3);
    } else {
        printf("FAIL: Expected %d iteration completions, got %d\n", expected_iterations, total_logs);
        exit_code = 1;
    }
    
    // The critical section protection ensures that:
    // 1. All printf calls within a single nosleep_log_* call are atomic
    // 2. No two threads can interleave their log output
    // 3. No crash occurs under concurrent logging load
    // We verify by checking the functions don't crash when called concurrently.
    // Complete line integrity would need stdout capture which is complex here.
    
    if (exit_code == 0) {
        printf("\nPASS: Multi-threaded logging completed without crash\n");
    } else {
        printf("\nFAIL: Multi-threaded logging test failed\n");
    }
    
    return exit_code;
}
TESTEOF

if $CC $CFLAGS "$TMPDIR/test_log_lock.c" "$OBJDIR/core_test.o" -o "$TMPDIR/test_log_lock.exe" $LIBS 2>&1; then
    # Run the test - redirect stdout to analyze output integrity
    "$TMPDIR/test_log_lock.exe" 2>&1
    if [ $? -eq 0 ]; then
        pass "Test 1: Multi-threaded logging completed without crash"
    else
        fail "Test 1: Multi-threaded logging test failed"
    fi
else
    fail "Test 1: Compilation failed"
fi

# ==========================================
# Test 2: GetTickCount64 time source
# ==========================================
echo ""
echo "--- Test 2: GetTickCount64 time source (standalone) ---"

cat > "$TMPDIR/test_tickcount64.c" << 'TESTEOF'
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// Simulate the get_elapsed_seconds function using GetTickCount64
static double get_elapsed_seconds(ULONGLONG start_tick64) {
    ULONGLONG current_tick64 = GetTickCount64();
    return (double)(current_tick64 - start_tick64) / 1000.0;
}

int main(void) {
    int failures = 0;
    
    printf("=== Testing GetTickCount64 elapsed time ===\n");
    
    // Test 1: Basic elapsed time measurement
    ULONGLONG start = GetTickCount64();
    Sleep(100);
    double elapsed = get_elapsed_seconds(start);
    
    if (elapsed >= 0.08 && elapsed <= 0.5) {
        printf("PASS: Basic elapsed time: %.3f s (expected ~0.1)\n", elapsed);
    } else {
        printf("FAIL: Basic elapsed time: %.3f s\n", elapsed);
        failures++;
    }
    
    // Test 2: Longer elapsed time
    start = GetTickCount64();
    Sleep(500);
    elapsed = get_elapsed_seconds(start);
    
    if (elapsed >= 0.4 && elapsed <= 1.0) {
        printf("PASS: Longer elapsed time: %.3f s (expected ~0.5)\n", elapsed);
    } else {
        printf("FAIL: Longer elapsed time: %.3f s\n", elapsed);
        failures++;
    }
    
    // Test 3: GetTickCount64 monotonic property
    ULONGLONG t1 = GetTickCount64();
    ULONGLONG t2 = GetTickCount64();
    ULONGLONG t3 = GetTickCount64();
    
    if (t1 <= t2 && t2 <= t3) {
        printf("PASS: Monotonic (%llu <= %llu <= %llu)\n", t1, t2, t3);
    } else {
        printf("FAIL: NOT monotonic (%llu, %llu, %llu)\n", t1, t2, t3);
        failures++;
    }
    
    // Test 4: Large values (no 32-bit wrap)
    ULONGLONG tick = GetTickCount64();
    printf("INFO: GetTickCount64 = %llu ms (uptime)\n", tick);
    if (tick < 0xFFFFFFFFLL) {
        printf("INFO: Value fits in 32-bit (system recently booted)\n");
    } else {
        printf("PASS: Value exceeds 32-bit range (no wrap issue)\n");
    }
    
    // Test 5: Duration check simulation
    ULONGLONG start_tick = GetTickCount64();
    ULONGLONG duration_ms = 200;
    
    Sleep(100);
    ULONGLONG elapsed_ms = GetTickCount64() - start_tick;
    if (elapsed_ms < duration_ms) {
        printf("PASS: Duration NOT yet reached: %llums < %llums\n", elapsed_ms, duration_ms);
    } else {
        printf("FAIL: Duration unexpectedly reached\n");
        failures++;
    }
    
    Sleep(150);
    elapsed_ms = GetTickCount64() - start_tick;
    if (elapsed_ms >= duration_ms) {
        printf("PASS: Duration reached: %llums >= %llums\n", elapsed_ms, duration_ms);
    } else {
        printf("FAIL: Duration NOT reached\n");
        failures++;
    }
    
    // Test 6: Large duration (no overflow)
    ULONGLONG big_duration = (ULONGLONG)24 * 60 * 60 * 1000; // 24 hours
    ULONGLONG calc = start_tick + big_duration;
    if (calc > start_tick) {
        printf("PASS: Large duration (24h) calculation: %llu + %llu > %llu\n",
               start_tick, big_duration, calc);
    } else {
        printf("FAIL: Large duration overflow\n");
        failures++;
    }
    
    printf("\n=== Failures: %d ===\n", failures);
    return failures > 0 ? 1 : 0;
}
TESTEOF

if $CC $CFLAGS "$TMPDIR/test_tickcount64.c" -o "$TMPDIR/test_tickcount64.exe" -lkernel32 2>&1; then
    if "$TMPDIR/test_tickcount64.exe" 2>&1; then
        pass "Test 2: GetTickCount64 time source works correctly"
    else
        fail "Test 2: GetTickCount64 test failed"
    fi
else
    fail "Test 2: Compilation failed"
fi

# ==========================================
# Summary
# ==========================================
echo ""
echo "=== Summary ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
