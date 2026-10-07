#!/bin/bash
# Test: Verify DEBUG_LOG macro refactoring in tray.c
# 1. No getenv("NOSLEEP_DEBUG") + strcmp patterns remain
# 2. No unguarded printf in tray_init/tray_run
# 3. DEBUG_LOG and DEBUG_PRINT macros are defined

set -euo pipefail
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'
PASS=0
FAIL=0

try() {
    local desc="$1"
    shift
    if "$@"; then
        echo -e "${GREEN}PASS:${NC} $desc"
        PASS=$((PASS + 1))
    else
        echo -e "${RED}FAIL:${NC} $desc"
        FAIL=$((FAIL + 1))
    fi
}

# 1. No getenv("NOSLEEP_DEBUG") calls in tray.c
try "No getenv(\"NOSLEEP_DEBUG\") in tray.c" \
    test "$(grep -c 'getenv("NOSLEEP_DEBUG")' src/tray.c)" -eq 0

# 2. No printf(...) followed by fflush(stdout) in tray_init/tray_run
# Match the printf identifier, excluding sprintf, snprintf, fprintf, and swprintf.
# grep exits 1 when no matches exist; preserve other errors under set -e.
BARE_PRINTF=$(grep -Ec '(^|[^[:alnum:]_])printf[[:space:]]*[(]' src/tray.c || [[ $? -eq 1 ]])
try "No bare printf( calls remain (debug output replaced)" \
    test "$BARE_PRINTF" -eq 0

# 3. Verify DEBUG_LOG macro is in tray.h
try "DEBUG_LOG macro defined in tray.h" \
    grep -q '#define DEBUG_LOG' src/tray.h

# 4. Verify DEBUG_PRINT macro is in tray.h  
try "DEBUG_PRINT macro defined in tray.h" \
    grep -q '#define DEBUG_PRINT' src/tray.h

# 5. Verify DEBUG_LOG is used in tray.c
try "DEBUG_LOG is used in tray.c" \
    test "$(grep -c 'DEBUG_LOG(' src/tray.c)" -gt 10

# 6. Verify DEBUG_PRINT is used in tray.c
try "DEBUG_PRINT is used in tray.c" \
    test "$(grep -c 'DEBUG_PRINT(' src/tray.c)" -gt 5

# 7. Verify all fprintf(stderr, "[nosleep]") debug messages are gone
# (one remaining: the unconditional error message about failing to load icon)
FPRINTF_STDERR=$(grep -c 'fprintf(stderr' src/tray.c)
try "No fprintf(stderr) debug messages remain (only unconditional errors)" \
    test "$FPRINTF_STDERR" -eq 1

# 8. Verify no const char* debug = getenv lines remain
try "No const char* debug = getenv declarations remain" \
    test "$(grep -c 'const char.*debug.*getenv' src/tray.c)" -eq 0

# 9. Verify the macro uses static variable caching
try "DEBUG_LOG uses static variable for env caching" \
    grep -q 'static const char' src/tray.h

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
