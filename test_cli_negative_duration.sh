#!/bin/bash
# Regression test: explicit negative durations must reach the indefinite start path.

set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re

source = Path("src/main.c").read_text()

duration_branch = re.search(
    r'else if \(strcmp\(arg, "--duration"\).*?\n\s*}\n\s*else if',
    source,
    re.S,
)
assert duration_branch, "could not find the --duration parsing branch"
branch = duration_branch.group(0)
assert re.search(
    r"opts->duration\s*=\s*atoi\(value\)\s*;\s*"
    r"if\s*\(opts->duration\s*<\s*0\)\s*\{?\s*"
    r"opts->duration\s*=\s*0\s*;",
    branch,
), "an explicit negative duration must be normalized to 0 (indefinite)"

start_path = re.search(
    r"if\s*\(opts->duration\s*>=\s*0\)\s*\{\s*"
    r"tray_start_nosleep\(tray,\s*opts->duration\);",
    source,
)
assert start_path, "an indefinite duration must reach tray_start_nosleep"
print("PASS: explicit negative --duration starts sleep prevention indefinitely")
PY
