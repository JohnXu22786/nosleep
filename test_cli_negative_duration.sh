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
    r"if\s*\(\s*WideCharToMultiByte\(CP_UTF8,\s*0,\s*value_wide,\s*-1,\s*value,\s*"
    r"sizeof\(value\),\s*NULL,\s*NULL\)\s*==\s*0\s*\|\|\s*"
    r"!cli_parse_duration\(value,\s*&opts->duration\)\)\s*\{\s*"
    r"return fail_cli_parse\(error,\s*CLI_PARSE_ERROR_INVALID_VALUE,\s*option,\s*"
    r"value_wide,\s*L\"an integer number of minutes\"\);\s*\}\s*"
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
