#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

python3 - "$SCRIPT_DIR" "$TMPDIR_PATH/test_tray_atomic_loads" <<'PY'
import os
import shlex
import subprocess
import sys

script_dir, output_path = sys.argv[1:]
cc_command = shlex.split(os.environ.get("CC") or "cc")
if not cc_command:
    raise SystemExit("CC must contain a compiler command")

subprocess.run(
    cc_command
    + [
        "-std=c99",
        "-Wall",
        "-Wextra",
        f"-I{script_dir}/tests/win32_atomic_stubs",
        f"-I{script_dir}/src",
        f"{script_dir}/tests/test_tray_atomic_loads.c",
        "-o",
        output_path,
    ],
    check=True,
)
PY

"$TMPDIR_PATH/test_tray_atomic_loads"
