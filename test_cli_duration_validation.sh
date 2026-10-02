#!/bin/bash
# Regression test: malformed --duration values must be rejected instead of
# becoming 0 (the indefinite-session value).

set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re

source = Path("src/main.c").read_text()
branch_match = re.search(
    r'else if \(strcmp\(arg, "--duration"\).*?\n\s*}\n\s*else if',
    source,
    re.S,
)
assert branch_match, "could not find the --duration parsing branch"
branch = branch_match.group(0)
assert re.search(
    r"if\s*\(\s*WideCharToMultiByte\(CP_UTF8,\s*0,\s*argv\[\+\+i\],\s*-1,\s*value,\s*"
    r"sizeof\(value\),\s*NULL,\s*NULL\)\s*==\s*0\s*\)\s*return 1\s*;",
    branch,
), "the --duration branch must reject values that do not fit its UTF-8 buffer"
assert re.search(
    r"if\s*\(!cli_parse_duration\(value,\s*&opts->duration\)\)\s*return 1\s*;",
    branch,
), "the --duration branch must reject values that cli_parse_duration cannot parse"
assert "atoi(value)" not in branch, "--duration must not silently accept atoi's numeric-prefix behavior"
PY

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

cat > "$TMPDIR/test_cli_duration.c" <<'C'
#include <assert.h>
#include <stdio.h>

#include "cli_duration.h"

int main(void) {
    int duration = -99;

    assert(!cli_parse_duration("abc", &duration));
    assert(!cli_parse_duration("20minutes", &duration));
    assert(cli_parse_duration("30", &duration));
    assert(duration == 30);

    puts("PASS: duration parsing rejects malformed values and accepts integers");
    return 0;
}
C

gcc -std=c99 -Wall -Wextra -Werror -Isrc "$TMPDIR/test_cli_duration.c" -o "$TMPDIR/test_cli_duration"
"$TMPDIR/test_cli_duration"
