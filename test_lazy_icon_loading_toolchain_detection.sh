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
mkdir -p "$FIXTURE_DIR/tests/win32_stubs"

cp "$ROOT_DIR/test_lazy_icon_loading.sh" "$FIXTURE_DIR/test_lazy_icon_loading.sh"
cp "$ROOT_DIR/test_tray_countdown_icon.sh" "$FIXTURE_DIR/test_tray_countdown_icon.sh"
cp "$ROOT_DIR/src/tray_countdown_icon.h" "$FIXTURE_DIR/src/tray_countdown_icon.h"
cp "$ROOT_DIR/tests/test_tray_countdown_icon.c" "$FIXTURE_DIR/tests/test_tray_countdown_icon.c"
cp "$ROOT_DIR/tests/win32_stubs/windows.h" "$FIXTURE_DIR/tests/win32_stubs/windows.h"
cat > "$FIXTURE_DIR/src/tray.h" <<'EOF'
#define TRAY_COUNTDOWN_NUMBERED_ICON_COUNT 61
typedef struct NoSleepTray {
    HICON hIconNumbered[TRAY_COUNTDOWN_NUMBERED_ICON_COUNT];
} NoSleepTray;
EOF
cat > "$FIXTURE_DIR/src/tray.c" <<'EOF'
static void tray_create_icons(NoSleepTray* tray);
static void tray_destroy_icons(NoSleepTray* tray);

static void tray_create_icons(NoSleepTray* tray) {
    (void)tray;
}

static void tray_destroy_icons(NoSleepTray* tray) {
    tray_destroy_countdown_icon_cache(tray->hIconNumbered, DestroyIcon);
}

void tray_destroy(NoSleepTray* tray) {
    tray_stop_nosleep(tray);
    tray_stop_countdown(tray);
    tray_destroy_icons(tray);
}

bool tray_init(NoSleepTray* tray) {
    return tray != NULL;
}

static void tray_create(NoSleepTray* tray) {
    memset(tray, 0, sizeof(*tray));
}

static void tray_update_icon(NoSleepTray* tray, int countdown_seconds,
                             bool countdown_blink_state) {
    tray->nid.hIcon = tray_countdown_display_icon(
        tray->hIconNumbered, countdown_seconds, countdown_blink_state,
        tray->hIconCountdownBlank, tray->hIconDefault, create_numbered_icon);
    int display_number = 1;
    if (tray->hIconNumbered[display_number] == NULL) {
        tray->hIconNumbered[display_number] = create_numbered_icon(display_number);
    }
}
EOF

for tool in as awk bash cc cut dirname grep head ld mktemp python3 rm sed; do
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

lazy_test_output() {
    (cd "$TMP_DIR" && PATH="$FAKE_BIN" \
        CC="$FAKE_BIN/cc" \
        FAKE_MAKE_DATABASE=$'CC = gcc\nRC = windres' \
        FAKE_MAKE_LOG="$MAKE_LOG" \
        FAKE_MAKE_PROJECT_DIR="$FIXTURE_DIR" \
        /bin/bash "$FIXTURE_DIR/test_lazy_icon_loading.sh")
}

cp "$FIXTURE_DIR/src/tray.c" "$TMP_DIR/tray.c.base"
python3 - "$FIXTURE_DIR/src/tray.c" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
definition = "static void tray_create_icons(NoSleepTray* tray) {\n"
assert source.count(definition) == 1
source = source.replace(
    definition,
    definition
    + "    for (int i = 0; i < 60; i++) {\n"
    + "        // Keep this call away from the adjacent-line source probe.\n"
    + "        HICON icon = create_numbered_icon(i);\n"
    + "        (void)icon;\n"
    + "    }\n",
)
path.write_text(source)
PY

if output="$(lazy_test_output 2>&1)"; then
    echo "FAIL: lazy icon test accepted eager creation in the tray_create_icons definition"
    echo "$output"
    exit 1
elif [[ "$output" != *"tray_create_icons still contains eager creation loop"* ]]; then
    echo "FAIL: lazy icon test did not inspect the tray_create_icons definition after its forward declaration"
    echo "$output"
    exit 1
fi
mv "$TMP_DIR/tray.c.base" "$FIXTURE_DIR/src/tray.c"
echo "PASS: lazy icon test rejects eager creation in the function definition after a forward declaration"

if ! output="$(cd "$TMP_DIR" && PATH="$FAKE_BIN" \
    CC="$FAKE_BIN/cc" \
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

cat > "$FAKE_BIN/windres" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "$FAKE_BIN/windres"

: > "$MAKE_LOG"
if ! output="$(cd "$TMP_DIR" && PATH="$FAKE_BIN" \
    CC="$FAKE_BIN/cc" \
    FAKE_MAKE_DATABASE=$'CC = gcc\nRC = windres' \
    FAKE_MAKE_LOG="$MAKE_LOG" \
    FAKE_MAKE_PROJECT_DIR="$FIXTURE_DIR" \
    /bin/bash "$FIXTURE_DIR/test_lazy_icon_loading.sh")"; then
    echo "FAIL: native compiler should cleanly skip when both tools are present"
    echo "$output"
    exit 1
fi

if [[ "$output" != *"SKIP: Compilation test"* || "$output" != *"does not target Windows"* ]]; then
    echo "FAIL: non-Windows compiler target should produce an explicit compile-check skip"
    echo "$output"
    exit 1
fi

if [[ -s "$MAKE_LOG" ]]; then
    echo "FAIL: make clean/build ran with a native compiler and resource compiler"
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
    CC="$FAKE_BIN/cc" \
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
