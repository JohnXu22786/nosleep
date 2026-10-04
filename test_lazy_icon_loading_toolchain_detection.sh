#!/usr/bin/env bash
# Test: the lazy icon test skips compilation without its configured Windows toolchain.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

FAKE_BIN="$TMP_DIR/bin"
MAKE_LOG="$TMP_DIR/make.log"
FIXTURE_DIR="$TMP_DIR/project"
mkdir -p "$FAKE_BIN" "$FIXTURE_DIR/src" "$FIXTURE_DIR/toolchain"

cp "$ROOT_DIR/test_lazy_icon_loading.sh" "$FIXTURE_DIR/test_lazy_icon_loading.sh"
cat > "$FIXTURE_DIR/src/tray.c" <<'EOF'
static void tray_create_icons(void) {
    if (hIconNumbered[countdown_seconds] == NULL) {
        tray->hIconNumbered[countdown_seconds] = create_numbered_icon(countdown_seconds);
    }
    if (hIconNumbered[display_number] == NULL) {
        tray->hIconNumbered[display_number] = create_numbered_icon(display_number);
    }
}
static void tray_destroy_icons(void) {
    if (tray->hIconNumbered[i]) DestroyIcon(tray->hIconNumbered[i]);
}
static void tray_create(void) {
    memset(tray, 0, sizeof(*tray));
}
EOF

for tool in awk bash cut dirname grep head sed; do
    ln -s "$(command -v "$tool")" "$FAKE_BIN/$tool"
done

cat > "$FAKE_BIN/make" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [[ " $* " == *" -pn "* ]]; then
    if [[ "$PWD" == "$FAKE_MAKE_PROJECT_DIR" ]]; then
        printf '%s\n' "$FAKE_MAKE_DATABASE"
    fi
else
    if (($#)); then
        printf '%s\n' "$*" >> "$FAKE_MAKE_LOG"
    else
        printf '<default>\n' >> "$FAKE_MAKE_LOG"
    fi
fi
EOF

cat > "$FAKE_BIN/gcc" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == "-dumpmachine" ]]; then
    echo x86_64-linux-gnu
    exit 0
fi
exit 1
EOF

chmod +x "$FAKE_BIN/make" "$FAKE_BIN/gcc"

if ! output="$(cd "$TMP_DIR" && PATH="$FAKE_BIN" \
    FAKE_MAKE_DATABASE=$'CC = gcc\nRC = windres' \
    FAKE_MAKE_LOG="$MAKE_LOG" \
    FAKE_MAKE_PROJECT_DIR="$FIXTURE_DIR" \
    /bin/bash "$FIXTURE_DIR/test_lazy_icon_loading.sh")"; then
    echo "FAIL: lazy icon test returned nonzero in the simulated missing-toolchain environment"
    echo "$output"
    exit 1
fi

if [[ "$output" != *"SKIP: Compilation test"* ]]; then
    echo "FAIL: missing Windows toolchain should produce an explicit compile-check skip"
    echo "$output"
    exit 1
fi

if [[ "$output" != *"Windows toolchain unavailable"* ]]; then
    echo "FAIL: skip should identify the missing Windows toolchain"
    echo "$output"
    exit 1
fi

if [[ -s "$MAKE_LOG" ]]; then
    echo "FAIL: make clean/build ran without the configured Windows toolchain"
    cat "$MAKE_LOG"
    exit 1
fi

cat > "$FIXTURE_DIR/toolchain/mingw-gcc" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == "-dumpmachine" ]]; then
    echo x86_64-w64-mingw32
    exit 0
fi
exit 1
EOF
cat > "$FIXTURE_DIR/toolchain/mingw-windres" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "$FIXTURE_DIR/toolchain/mingw-gcc" "$FIXTURE_DIR/toolchain/mingw-windres"

: > "$MAKE_LOG"
if ! output="$(cd "$TMP_DIR" && PATH="$FAKE_BIN" \
    FAKE_MAKE_DATABASE=$'CC = ./toolchain/mingw-gcc\nRC = ./toolchain/mingw-windres' \
    FAKE_MAKE_LOG="$MAKE_LOG" \
    FAKE_MAKE_PROJECT_DIR="$FIXTURE_DIR" \
    /bin/bash "$FIXTURE_DIR/test_lazy_icon_loading.sh")"; then
    echo "FAIL: configured Windows toolchain should let the compile check run"
    echo "$output"
    exit 1
fi

if [[ "$output" != *"PASS: Project compiles successfully with make"* ]]; then
    echo "FAIL: compile check did not use the configured Windows toolchain"
    echo "$output"
    exit 1
fi

if [[ "$(cat "$MAKE_LOG")" != $'clean\n<default>' ]]; then
    echo "FAIL: expected make clean and build only after finding both configured tools"
    cat "$MAKE_LOG"
    exit 1
fi

echo "PASS: compilation skips without the Windows toolchain and uses configured CC/RC when available"
