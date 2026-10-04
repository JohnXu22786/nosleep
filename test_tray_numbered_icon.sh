#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

"${CC:-cc}" -std=c99 -Wall -Wextra \
    -I"$SCRIPT_DIR/tests/win32_stubs" -I"$SCRIPT_DIR/src" \
    "$SCRIPT_DIR/tests/test_tray_numbered_icon.c" \
    -o "$TMPDIR_PATH/test_tray_numbered_icon"
"$TMPDIR_PATH/test_tray_numbered_icon"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import sys

tray = (Path(sys.argv[1]) / "src/tray.c").read_text()
calls = re.findall(
    r"tray_release_numbered_icon\s*\(\s*&tray->hIconCurrentNumbered\s*,\s*"
    r"tray->current_number\s*,\s*tray->hIconActive\s*,\s*DestroyIcon\s*\)",
    tray,
)
assert len(calls) == 2, (
    "number changes and tray teardown must both use numbered-icon ownership cleanup"
)
assert "tray->hIconCurrentNumbered = tray->hIconActive;" in tray, (
    "failed numbered icon creation must continue to use the active icon fallback"
)
PY
echo "PASS: tray_update_icon and tray teardown use numbered icon ownership cleanup"
