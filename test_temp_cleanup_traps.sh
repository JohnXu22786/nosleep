#!/bin/bash
# Regression test: EXIT cleanup must preserve paths with spaces as one argument.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TEST_ROOT="$(TMPDIR=/tmp mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

MOCK_BIN="$TEST_ROOT/mock-bin"
mkdir -p "$MOCK_BIN"
cat > "$MOCK_BIN/mktemp" <<'MOCK'
#!/bin/bash
set -euo pipefail

if [[ "${1-}" != "-d" ]]; then
    echo "unexpected mktemp arguments" >&2
    exit 2
fi

mkdir -p -- "$CLEANUP_TEST_TMPDIR"
printf '%s\n' "$CLEANUP_TEST_TMPDIR"
MOCK
chmod +x "$MOCK_BIN/mktemp"

failures=0
for script in test_cli_batch_mode.sh test_cli_options_refactor.sh test_notification_groups.sh; do
    label="${script%.sh}"
    prefix="$TEST_ROOT/$label"
    temp_dir="$prefix temp dir"
    sentinel="$prefix/keep-me"
    mkdir -p "$prefix"
    printf 'preserve me\n' > "$sentinel"

    trap_line="$(awk '/^trap .* EXIT$/ { print NR; exit }' "$SCRIPT_DIR/$script")"
    if [[ -z "$trap_line" ]]; then
        echo "FAIL: $script has no EXIT trap"
        failures=$((failures + 1))
        continue
    fi

    if (
        cd "$TEST_ROOT"
        export PATH="$MOCK_BIN:$PATH"
        export CLEANUP_TEST_TMPDIR="$temp_dir"
        source <(sed -n "1,${trap_line}p" "$SCRIPT_DIR/$script")
    ); then
        if [[ -f "$sentinel" && ! -e "$temp_dir" ]]; then
            echo "PASS: $script removes its spaced temp directory and preserves the prefix"
        else
            echo "FAIL: $script did not clean up safely"
            failures=$((failures + 1))
        fi
    else
        echo "FAIL: could not exercise $script EXIT trap"
        failures=$((failures + 1))
    fi
done

if (( failures > 0 )); then
    exit 1
fi
