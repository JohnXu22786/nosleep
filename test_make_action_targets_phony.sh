#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

touch "$TMPDIR/test-updater-url-policy"

set +e
make -q -C "$TMPDIR" -f "$SCRIPT_DIR/Makefile" test-updater-url-policy
status=$?
set -e

if [[ "$status" -ne 1 ]]; then
    echo "FAIL: make treated test-updater-url-policy as up to date when a same-named file exists (status $status)" >&2
    exit 1
fi

echo "PASS: test-updater-url-policy remains an action target when a same-named file exists"
