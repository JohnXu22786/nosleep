#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

"${CC:-cc}" -std=c99 -Wall -Wextra -I"$SCRIPT_DIR/src" \
    "$SCRIPT_DIR/tests/test_tray_stop_guard.c" \
    -o "$TMPDIR_PATH/test_tray_stop_guard"
"$TMPDIR_PATH/test_tray_stop_guard"
