#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

"${CC:-cc}" -std=c99 -Wall -Wextra \
    -I"$SCRIPT_DIR/tests/win32_stubs" -I"$SCRIPT_DIR/src" \
    "$SCRIPT_DIR/tests/test_tray_indefinite_icon.c" \
    -o "$TMPDIR_PATH/test_tray_indefinite_icon"
"$TMPDIR_PATH/test_tray_indefinite_icon"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import sys

tray = (Path(sys.argv[1]) / "src/tray.c").read_text()
assert re.search(
    r"tray_ensure_indefinite_icon\s*\(\s*&tray->hIconCurrentNumbered\s*,\s*"
    r"&tray->current_number\s*,\s*tray->hIconActive\s*,\s*"
    r"create_numbered_icon\s*,\s*DestroyIcon",
    tray,
), "tray_update_icon must use the tested indefinite-icon transition helper"
assert re.search(
    r"if\s*\(number\s*<\s*0\)\s*\{\s*wcscpy\(text,\s*L\"\\u221E\"\);",
    tray,
), "the indefinite icon must use a wide U+221E text string"
assert tray.count("GetTextExtentPoint32W(") == 2 and tray.count("TextOutW(") == 2, (
    "numbered-icon text must be measured and drawn with wide-character APIs"
)
PY
echo "PASS: tray_update_icon uses the tested indefinite icon transition helper"
echo "PASS: indefinite icon text uses wide-character drawing APIs"
