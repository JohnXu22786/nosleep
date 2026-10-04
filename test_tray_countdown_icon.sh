#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

"${CC:-cc}" -std=c99 -Wall -Wextra \
    -I"$SCRIPT_DIR/tests/win32_stubs" -I"$SCRIPT_DIR/src" \
    "$SCRIPT_DIR/tests/test_tray_countdown_icon.c" \
    -o "$TMPDIR_PATH/test_tray_countdown_icon"
"$TMPDIR_PATH/test_tray_countdown_icon"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
tray = (root / "src/tray.c").read_text()
tray_header = (root / "src/tray.h").read_text()
assert re.search(
    r"tray_countdown_display_icon\s*\(\s*tray->hIconNumbered\s*,\s*"
    r"countdown_seconds\s*,\s*countdown_blink_state\s*,\s*"
    r"tray->hIconCountdownBlank\s*,\s*tray->hIconDefault\s*,\s*"
    r"create_numbered_icon\s*\)",
    tray,
), "tray_update_icon must use the tested countdown icon selection/cache helper"
assert "HICON hIconNumbered[TRAY_COUNTDOWN_NUMBERED_ICON_COUNT]" in tray_header, (
    "the numbered icon cache must own the complete 0-60 countdown range"
)
assert re.search(
    r"tray_destroy_countdown_icon_cache\s*\(\s*tray->hIconNumbered\s*,\s*"
    r"DestroyIcon\s*\)",
    tray,
), "tray_destroy_icons must release cached countdown icons"
destroy_body = tray.split("void tray_destroy(NoSleepTray* tray) {", 1)[1].split(
    "\nbool tray_init(", 1
)[0]
session_stop_position = destroy_body.find("tray_stop_nosleep(tray")
stop_position = destroy_body.find("tray_stop_countdown(tray)")
icon_cleanup_position = destroy_body.find("tray_destroy_icons(tray)")
assert session_stop_position >= 0 and session_stop_position < stop_position < icon_cleanup_position, (
    "tray_destroy must join a naturally finishing countdown before freeing its icons"
)
PY
echo "PASS: tray_update_icon uses the tested countdown icon cache"
echo "PASS: tray_destroy_icons releases countdown icon cache entries"
