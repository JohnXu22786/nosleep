#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== Test: production PATH registry behavior ==="
bash "$SCRIPT_DIR/test_path_unicode.sh"

echo ""
echo "=== Test: PATH settings preference behavior ==="
bash "$SCRIPT_DIR/test_cli_settings_failures.sh"
