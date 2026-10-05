#!/bin/bash
# Regression test: initial updater downloads retain query parameters from asset URLs.

set -euo pipefail

source_path="${1:-src/updater.c}"
python3 - "$source_path" <<'PY'
import re
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text(encoding="utf-8")
definition = source.rfind("static bool download_file(const char* url,")
if definition < 0:
    raise SystemExit("FAIL: download_file definition was not found")
download_file = source[definition:]

checks = {
    "URL extra-info is parsed": r"urlComp\.lpszExtraInfo\s*=\s*urlExtraInfo\s*;",
    "path and query are combined": (
        r"updater_build_request_target\s*\(\s*urlPath\s*,\s*urlExtraInfo\s*,\s*requestTarget\s*,"
    ),
    "initial request uses the combined target": (
        r"WinHttpOpenRequest\s*\(\s*hConnect\s*,\s*L\"GET\"\s*,\s*requestTarget\s*,"
    ),
}

for description, pattern in checks.items():
    if not re.search(pattern, download_file):
        raise SystemExit(f"FAIL: {description}")
    print(f"PASS: {description}")
PY
