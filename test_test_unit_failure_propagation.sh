#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT

cat > "$TMPDIR_PATH/fake-cc" <<'FAKE_CC'
#!/bin/bash
set -euo pipefail

output=""
updater_test=false
while (($#)); do
    if [[ "$1" == "-o" ]]; then
        output="$2"
        shift 2
        continue
    fi
    if [[ "$1" == "tests/test_updater.c" ]]; then
        updater_test=true
    fi
    shift
done

if [[ -z "$output" ]]; then
    echo "fake compiler received no output path" >&2
    exit 2
fi

if [[ "$updater_test" == true ]]; then
    cat > "$output" <<'FAILING_TEST'
#!/bin/sh
echo "simulated updater assertion failure" >&2
exit 1
FAILING_TEST
else
    cat > "$output" <<'PASSING_TEST'
#!/bin/sh
exit 0
PASSING_TEST
fi
chmod +x "$output"
FAKE_CC
chmod +x "$TMPDIR_PATH/fake-cc"

if make -C "$SCRIPT_DIR" test-unit "CC=$TMPDIR_PATH/fake-cc" >"$TMPDIR_PATH/output" 2>&1; then
    cat "$TMPDIR_PATH/output"
    echo "test-unit succeeded even though the updater test executable fails" >&2
    exit 1
fi

if ! grep -Fq "simulated updater assertion failure" "$TMPDIR_PATH/output"; then
    cat "$TMPDIR_PATH/output"
    echo "test-unit failed without running the updater test executable" >&2
    exit 1
fi

echo "test-unit propagates updater test failures"
