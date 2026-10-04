#!/bin/bash
# Regression test: reject CLI refresh intervals that overflow millisecond waits.

set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re

main = Path("src/main.c").read_text()
branch_match = re.search(
    r'else if \(strcmp\(arg, "--interval".*?\n\s*}\n\s*else if',
    main,
    re.S,
)
assert branch_match, "could not find the --interval parsing branch"
assert re.search(
    r"if\s*\(\s*WideCharToMultiByte\(CP_UTF8,\s*0,\s*value_wide,\s*-1,\s*value,\s*"
    r"sizeof\(value\),\s*NULL,\s*NULL\)\s*==\s*0\s*\|\|\s*"
    r"!cli_parse_refresh_interval\(value,\s*&opts->interval\)\)\s*\{\s*"
    r"return fail_cli_parse\(error,\s*CLI_PARSE_ERROR_INVALID_VALUE,\s*option,\s*"
    r"value_wide,\s*L\"a positive integer number of seconds\"\);\s*\}",
    branch_match.group(0),
), "the --interval branch must reject malformed values and retain them for diagnostics"
assert re.search(
    r"cli_parse_refresh_interval\(value,\s*&opts->interval\)", branch_match.group(0)
), "the --interval branch must safely parse and bound millisecond waits"
assert re.search(
    r"tray->refresh_interval_seconds\s*=\s*opts->interval\s*;", main
), "run_tray_mode must transfer the parsed interval to tray state"

tray = Path("src/tray.c").read_text()
assert re.search(
    r"tray->refresh_interval_seconds\s*,\s*// interval_seconds", tray
), "the tray worker must pass the configured interval to nosleep_run"
PY

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

cat > "$TMPDIR/test_cli_interval.c" <<'C'
#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "cli_interval.h"

int main(void) {
    int interval = -1;

    assert(cli_parse_refresh_interval("1", &interval) && interval == 1);
    assert(cli_parse_refresh_interval("20", &interval) && interval == 20);
    assert(cli_parse_refresh_interval("2147483", &interval));
    assert(interval == INT_MAX / 1000);
    assert(!cli_parse_refresh_interval("2147484", &interval));
    assert(!cli_parse_refresh_interval("4294968", &interval));
    assert(!cli_parse_refresh_interval("2147483648", &interval));
    assert(!cli_parse_refresh_interval("999999999999999999999999", &interval));
    assert(!cli_parse_refresh_interval("0", &interval));
    assert(!cli_parse_refresh_interval("-1", &interval));
    assert(!cli_parse_refresh_interval("20 seconds", &interval));

    puts("PASS: refresh interval parsing rejects invalid and overflowing values");
    return 0;
}
C

gcc -std=c99 -Wall -Wextra -Werror -Isrc "$TMPDIR/test_cli_interval.c" -o "$TMPDIR/test_cli_interval"
"$TMPDIR/test_cli_interval"
