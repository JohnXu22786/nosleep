#!/usr/bin/env bash
# Test: changing VERSION rebuilds objects and resources that embed the version.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

PROJECT_DIR="$TMP_DIR/project"
FAKE_BIN="$TMP_DIR/fake-bin"
BUILD_LOG="$TMP_DIR/build.log"

mkdir -p "$PROJECT_DIR/src" "$FAKE_BIN"
cp "$ROOT_DIR/Makefile" "$PROJECT_DIR/Makefile"
cp "$ROOT_DIR/src/resources.rc" "$PROJECT_DIR/src/resources.rc"

for source in core tray main notify_groups updater updater_logic updater_response_read updater_pe cJSON; do
    : > "$PROJECT_DIR/src/$source.c"
done

cat > "$FAKE_BIN/gcc" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

args="$*"
printf 'gcc %s\n' "$args" >> "$BUILD_LOG"

output=""
while (($#)); do
    if [[ "$1" == "-o" ]]; then
        shift
        output="$1"
    fi
    shift
done

[[ -n "$output" ]]
printf '%s\n' "$args" > "$output"
EOF

cat > "$FAKE_BIN/windres" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

args="$*"
printf 'windres %s\n' "$args" >> "$BUILD_LOG"

output=""
while (($#)); do
    if [[ "$1" == "-o" ]]; then
        shift
        output="$1"
    fi
    shift
done

[[ -n "$output" ]]
printf '%s\n' "$args" > "$output"
EOF

chmod +x "$FAKE_BIN/gcc" "$FAKE_BIN/windres"

run_make() {
    (cd "$PROJECT_DIR" && PATH="$FAKE_BIN:$PATH" BUILD_LOG="$BUILD_LOG" make -s "VERSION=$1")
}

run_make 1.2.3

gcc_calls="$(grep -c '^gcc ' "$BUILD_LOG" || true)"
windres_calls="$(grep -c '^windres ' "$BUILD_LOG" || true)"
if [[ "$gcc_calls" != "10" || "$windres_calls" != "1" ]]; then
    echo "FAIL: expected initial build to run 9 compiles, 1 link, and 1 resource compile; got $gcc_calls gcc and $windres_calls windres calls"
    exit 1
fi

run_make 1.2.3
gcc_calls="$(grep -c '^gcc ' "$BUILD_LOG" || true)"
windres_calls="$(grep -c '^windres ' "$BUILD_LOG" || true)"
if [[ "$gcc_calls" != "10" || "$windres_calls" != "1" ]]; then
    echo "FAIL: repeating the same VERSION should not rebuild outputs; got $gcc_calls gcc and $windres_calls windres calls total"
    exit 1
fi

run_make 2.3.4

gcc_calls="$(grep -c '^gcc ' "$BUILD_LOG" || true)"
windres_calls="$(grep -c '^windres ' "$BUILD_LOG" || true)"
if [[ "$gcc_calls" != "20" || "$windres_calls" != "2" ]]; then
    echo "FAIL: changing VERSION should rebuild 9 objects, the executable, and the resource; got $gcc_calls gcc and $windres_calls windres calls total"
    exit 1
fi

new_version_compile_calls="$(grep -F -c -- '-DVERSION_STR="2.3.4"' "$BUILD_LOG" || true)"
if [[ "$new_version_compile_calls" != "9" ]]; then
    echo "FAIL: expected 9 object compiles with VERSION_STR=2.3.4; got $new_version_compile_calls"
    exit 1
fi

if ! grep -Fq 'FILEVERSION     2,3,4,0' "$PROJECT_DIR/obj/resources_built.rc" || \
   ! grep -Fq '"FileVersion", "2.3.4.0"' "$PROJECT_DIR/obj/resources_built.rc"; then
    echo "FAIL: generated resource source does not contain VERSION=2.3.4"
    exit 1
fi

echo "PASS: changing VERSION rebuilds all objects and resource output with the new version"
