#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workflow="$repo_dir/.github/workflows/release.yml"
rename_command=$(awk '
  /^      - name: Rename executable$/ { in_rename = 1; next }
  in_rename && /^      - name:/ { exit }
  in_rename && /^        run: / {
    sub(/^        run: /, "")
    print
    exit
  }
' "$workflow")

if [[ -z "$rename_command" ]]; then
  echo "Could not find the release executable rename command" >&2
  exit 1
fi

malicious_tag='v$(touch${IFS}marker)'
if ! git -C "$repo_dir" check-ref-format "refs/tags/$malicious_tag"; then
  echo "The regression tag is not a valid Git tag" >&2
  exit 1
fi

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT
mkdir -p "$temp_dir/bin"

run_rename() {
  local tag=$1
  local command=${rename_command//\$\{\{ github.ref_name \}\}/$tag}

  printf 'binary\n' > "$temp_dir/bin/nosleep.exe"
  (
    cd "$temp_dir"
    GITHUB_REF_NAME="$tag" bash -c "$command"
  )
}

run_rename "$malicious_tag"
if [[ -e "$temp_dir/marker" ]]; then
  echo "The release tag executed shell syntax during the rename" >&2
  exit 1
fi
literal_asset_name="nosleep-${malicious_tag}.exe"
if [[ ! -f "$temp_dir/$literal_asset_name" ]]; then
  echo "The release rename did not preserve the tag as literal filename data" >&2
  exit 1
fi

run_rename 'v1.2.3'
if [[ ! -f "$temp_dir/nosleep-v1.2.3.exe" ]]; then
  echo "The release rename changed the asset name for an ordinary version tag" >&2
  exit 1
fi

echo "Release asset tag safety checks passed"

build_command=$(awk '
  /^      - name: Build$/ { in_build = 1; next }
  in_build && /^      - name:/ { exit }
  in_build && /^        run: \|$/ { in_run = 1; next }
  in_run && /^          / {
    sub(/^          /, "")
    print
    next
  }
  in_run { exit }
' "$workflow")

if [[ -z "$build_command" ]]; then
  echo "Could not find the release build command" >&2
  exit 1
fi

fake_bin="$temp_dir/fake-bin"
make_log="$temp_dir/make.log"
mkdir -p "$fake_bin"
cat > "$fake_bin/make" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$MAKE_LOG"
EOF
chmod +x "$fake_bin/make"

run_build() {
  local tag=$1
  local log=$2
  local output=$3

  GITHUB_REF_NAME="$tag" \
    PATH="$fake_bin:$PATH" \
    MAKE_LOG="$log" \
    bash -e -o pipefail -c "$build_command" > "$output" 2>&1
}

unsafe_version_tag='v1.2.3;id;#'
if ! git -C "$repo_dir" check-ref-format "refs/tags/$unsafe_version_tag"; then
  echo "The release version regression tag is not a valid Git tag" >&2
  exit 1
fi

if run_build "$unsafe_version_tag" "$make_log" "$temp_dir/unsafe-build.log"; then
  echo "The release build accepted a tag containing shell metacharacters" >&2
  exit 1
fi
if [[ -s "$make_log" ]]; then
  echo "Make ran before the invalid release version was rejected" >&2
  exit 1
fi
if ! grep -Fq 'Invalid release tag' "$temp_dir/unsafe-build.log"; then
  echo "The invalid release tag failure did not explain how to fix the tag" >&2
  cat "$temp_dir/unsafe-build.log" >&2
  exit 1
fi

for supported_tag in v1.2.3 v2.0.0-fix v3.4.5-beta.2; do
  : > "$make_log"
  if ! run_build "$supported_tag" "$make_log" "$temp_dir/supported-build.log"; then
    echo "The release build rejected supported tag $supported_tag" >&2
    cat "$temp_dir/supported-build.log" >&2
    exit 1
  fi
  if ! grep -Fq "VERSION=${supported_tag#v}" "$make_log"; then
    echo "The release build did not pass the validated version for $supported_tag to Make" >&2
    exit 1
  fi
done

echo "Release version validation checks passed"
