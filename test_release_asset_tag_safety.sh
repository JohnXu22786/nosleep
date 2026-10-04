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
