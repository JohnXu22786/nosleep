#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$SCRIPT_DIR" <<'PY'
from pathlib import Path
import re
import sys

tray = (Path(sys.argv[1]) / "src/tray.c").read_text()
general_tab = re.search(
    r'\bhGeneralTab\s*=\s*CreateWindowEx\(\s*([^,]+),\s*"STATIC"\s*,\s*NULL\s*,',
    tray,
    re.S,
)
assert general_tab, "could not find the General tab container creation"
assert re.search(r"\bWS_EX_CONTROLPARENT\b", general_tab.group(1)), (
    "the General tab container must expose its child controls to dialog keyboard navigation"
)
PY
