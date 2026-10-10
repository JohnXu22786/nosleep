#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

TMPDIR_PATH="$TEST_ROOT/temp dir"
INCLUDE_PATH="$TMPDIR_PATH/include path"
WRAPPER_PATH="$TMPDIR_PATH/compiler wrapper"
mkdir -p "$INCLUDE_PATH"

cat > "$WRAPPER_PATH" <<'COMPILER'
#!/bin/bash
set -euo pipefail

if [[ "${1:-}" != "--require-cc-argument" ]]; then
    echo "compiler wrapper did not receive its required CC argument" >&2
    exit 1
fi
if [[ "${2:-}" != "-isystem" || "${3:-}" != "$TEST_EXPECTED_CC_INCLUDE" ]]; then
    echo "compiler wrapper did not receive the quoted CC include path intact" >&2
    exit 1
fi
shift
exec "$TEST_REAL_CC" "$@"
COMPILER
chmod +x "$WRAPPER_PATH"

TEST_REAL_CC="$(command -v cc)" \
TEST_EXPECTED_CC_INCLUDE="$INCLUDE_PATH" \
TMPDIR="$TMPDIR_PATH" \
CC="\"$WRAPPER_PATH\" --require-cc-argument -isystem \"$INCLUDE_PATH\"" \
    bash "$SCRIPT_DIR/test_tray_atomic_loads.sh"

echo "tray atomic test honors quoted CC arguments and paths with spaces"
