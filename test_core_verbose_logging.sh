#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

CC="${CC:-gcc}"
"$CC" -std=c99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$SCRIPT_DIR/tests/core_stubs" -I"$SCRIPT_DIR/src" \
    -Dfflush=test_core_fflush \
    "$SCRIPT_DIR/tests/test_core_verbose_logging.c" \
    "$SCRIPT_DIR/src/core.c" -Wl,--gc-sections \
    -o "$TMPDIR_PATH/test_core_verbose_logging"

OUTPUT="$("$TMPDIR_PATH/test_core_verbose_logging")"
printf '%s\n' "$OUTPUT"
printf '%s\n' "$OUTPUT" | grep -Fq \
    '[12:34:56 UTC] INFO - Status: Active for 2m 5s (refresh #7)'
printf '%s\n' "$OUTPUT" | grep -Fq '[12:34:56 UTC] INFO - Regular status'
