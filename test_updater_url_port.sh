#!/bin/bash
# Regression test: updater connections use the port parsed from each HTTPS URL.

set -euo pipefail

source_path="${1:-src/updater.c}"
python3 - "$source_path" <<'PY'
import re
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text(encoding="utf-8")


def extract_function(signature):
    start = source.rfind(signature)
    if start < 0:
        raise SystemExit(f"FAIL: {signature} definition was not found")
    end = source.find("\n}\n", start)
    if end < 0:
        raise SystemExit(f"FAIL: could not locate the end of {signature}")
    return source[start:end]


checks = [
    (
        "initial asset connection uses the parsed URL port",
        extract_function("static bool download_file(const char* url,"),
        r"WinHttpConnect\s*\(\s*hSession\s*,\s*hostName\s*,\s*urlComp\.nPort\s*,\s*0\s*\)",
    ),
    (
        "redirect connection uses the parsed URL port",
        extract_function("static DWORD follow_redirects("),
        r"WinHttpConnect\s*\(\s*hSession\s*,\s*newHost\s*,\s*newUrlComp\.nPort\s*,\s*0\s*\)",
    ),
]

for description, function, pattern in checks:
    if not re.search(pattern, function):
        raise SystemExit(f"FAIL: {description}")
    print(f"PASS: {description}")
PY
