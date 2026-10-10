#!/usr/bin/env bash
# Accepted versions must preserve display strings and valid numeric resource fields.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
mkdir -p "$TMP_DIR/src"
cp "$ROOT_DIR/Makefile" "$TMP_DIR/Makefile"
cp "$ROOT_DIR/src/resources.rc" "$ROOT_DIR/src/resources.h" "$TMP_DIR/src/"
cp "$ROOT_DIR/no-sleeping_9260684.ico" "$TMP_DIR/"

for tag in v08.9.10 v8.09.10 v8.9.010 v8.9.10-001 v8.9.10-rc.01; do
    if bash "$ROOT_DIR/.github/scripts/validate-release-version.sh" "$tag" >/dev/null 2>&1; then
        echo "FAIL: accepted release tag with a leading-zero numeric identifier: $tag"
        exit 1
    fi
done
if [[ "$(bash "$ROOT_DIR/.github/scripts/validate-release-version.sh" v0.0.0-0)" != "0.0.0-0" ]]; then
    echo "FAIL: rejected a canonical release tag containing zero components"
    exit 1
fi

# CI supplies MinGW; generation assertions also run on hosts without that compiler.
resource_compiler="${RC:-x86_64-w64-mingw32-windres}"
if ! command -v "$resource_compiler" >/dev/null 2>&1; then
    if [[ -n "${RC:-}" ]]; then
        echo "FAIL: requested resource compiler $resource_compiler unavailable"
        exit 1
    fi
    echo "NOTE: $resource_compiler unavailable; checking generated resources only"
    resource_compiler=true
fi
for tag in v3.4.5 v3.4.5-beta.2 v3.4.5-rc-1; do
    version="$(bash "$ROOT_DIR/.github/scripts/validate-release-version.sh" "$tag")"
    make -s -C "$TMP_DIR" obj/resources.o "VERSION=$version" "RC=$resource_compiler"
    for field in FILEVERSION PRODUCTVERSION; do
        if ! grep -Eq "^$field[[:space:]]+3,4,5,0$" "$TMP_DIR/obj/resources_built.rc"; then
            echo "FAIL: $tag generated an invalid $field"
            exit 1
        fi
    done
    for field in FileVersion ProductVersion; do
        grep -Fq "\"$field\", \"$version.0\"" "$TMP_DIR/obj/resources_built.rc"
    done
done

version="$(bash "$ROOT_DIR/.github/scripts/validate-release-version.sh" v8.9.10)"
make -s -C "$TMP_DIR" obj/resources.o "VERSION=$version" "RC=$resource_compiler"
for field in FILEVERSION PRODUCTVERSION; do
    if ! grep -Eq "^$field[[:space:]]+8,9,10,0$" "$TMP_DIR/obj/resources_built.rc"; then
        echo "FAIL: Makefile generated an invalid $field for v8.9.10"
        exit 1
    fi
done
for field in FileVersion ProductVersion; do
    grep -Fq "\"$field\", \"$version.0\"" "$TMP_DIR/obj/resources_built.rc"
done

# Keep the README's manual build recipe in sync with the Makefile's decimal
# resource-version generation for canonical release components.
manual_commands="$TMP_DIR/manual-resource-version.sh"
sed -n '/^VERSION_RESOURCE_INPUT=/,/^sed .*obj\/resources_built.rc/p' "$ROOT_DIR/README.md" > "$manual_commands"
if [[ ! -s "$manual_commands" ]] || ! grep -q '^sed .*obj/resources_built.rc$' "$manual_commands"; then
    echo "FAIL: could not find the manual resource-generation commands in README.md"
    exit 1
fi
mkdir -p "$TMP_DIR/manual/src" "$TMP_DIR/manual/obj"
cp "$ROOT_DIR/src/resources.rc" "$TMP_DIR/manual/src/"
(
    cd "$TMP_DIR/manual"
    VERSION="$version" bash "$manual_commands"
)
for field in FILEVERSION PRODUCTVERSION; do
    if ! grep -Eq "^$field[[:space:]]+8,9,10,0$" "$TMP_DIR/manual/obj/resources_built.rc"; then
        echo "FAIL: manual recipe generated an invalid $field for v8.9.10"
        exit 1
    fi
done
for field in FileVersion ProductVersion; do
    grep -Fq "\"$field\", \"$version.0\"" "$TMP_DIR/manual/obj/resources_built.rc"
done

echo "PASS: Makefile and manual build generate valid numeric and display resource fields"
