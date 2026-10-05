#!/bin/bash
set -euo pipefail

TEST_TMPDIR=$(mktemp -d)
trap 'rm -rf "$TEST_TMPDIR"' EXIT

CC="${CC:-gcc}"
TEST_CFLAGS="-std=c99 -Wall -Wextra -O0 -g -Isrc"

echo "=== Test Suite: Core logging + time source ==="

# The deterministic Win32 stubs let this test compile and run production core.c
# on Linux while checking elapsed-time status output across a 32-bit tick wrap.
$CC $TEST_CFLAGS -Itests/core_stubs tests/test_core_log_time.c src/core.c \
    -o "$TEST_TMPDIR/test_core_log_time"
"$TEST_TMPDIR/test_core_log_time"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # Keep native thread coverage on Windows using real Win32 locks and threads.
        $CC $TEST_CFLAGS tests/test_core_log_time_windows.c src/core.c \
            -o "$TEST_TMPDIR/test_core_log_time_windows.exe" -lkernel32 -luser32
        if "$TEST_TMPDIR/test_core_log_time_windows.exe" >"$TEST_TMPDIR/windows-thread.log" 2>&1; then
            cat "$TEST_TMPDIR/windows-thread.log"
        else
            cat "$TEST_TMPDIR/windows-thread.log"
            exit 1
        fi
        ;;
    *)
        echo "SKIP: native Windows thread logging test requires Windows"
        ;;
esac
