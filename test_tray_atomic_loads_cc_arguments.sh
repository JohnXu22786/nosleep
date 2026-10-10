#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

cat > "$TMPDIR_PATH/compiler-wrapper" <<'COMPILER'
#!/bin/bash
set -euo pipefail

if [[ "${1:-}" != "--require-cc-argument" ]]; then
    echo "compiler wrapper did not receive its required CC argument" >&2
    exit 1
fi
shift
exec "$TEST_REAL_CC" "$@"
COMPILER
chmod +x "$TMPDIR_PATH/compiler-wrapper"

TEST_REAL_CC="$(command -v cc)" \
CC="$TMPDIR_PATH/compiler-wrapper --require-cc-argument" \
    bash "$SCRIPT_DIR/test_tray_atomic_loads.sh"

echo "tray atomic test honors CC arguments"
