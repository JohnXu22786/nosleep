#!/bin/bash
# Run the core duration-boundary regression against a virtual Win32 clock.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -I"$SCRIPT_DIR/tests/core_stubs" -I"$SCRIPT_DIR/src" \
    "$SCRIPT_DIR/tests/test_core_duration_boundary.c" \
    "$SCRIPT_DIR/src/core.c" -o "$TMPDIR_PATH/test_core_duration_boundary"
"$TMPDIR_PATH/test_core_duration_boundary"
