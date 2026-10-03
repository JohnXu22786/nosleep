#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

"${CC:-cc}" -std=c99 -Wall -Wextra \
    -I"$SCRIPT_DIR/tests/win32_stubs" -I"$SCRIPT_DIR/src" \
    "$SCRIPT_DIR/tests/test_notify_groups_migration.c" \
    "$SCRIPT_DIR/src/notify_groups.c" \
    -o "$TMPDIR_PATH/test_notify_groups_migration"
"$TMPDIR_PATH/test_notify_groups_migration"
