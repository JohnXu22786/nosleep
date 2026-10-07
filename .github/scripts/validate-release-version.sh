#!/usr/bin/env bash
set -euo pipefail

tag=${1:-}
version_pattern='^v[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9]+([.-][A-Za-z0-9]+)*)?$'

if [[ ! "$tag" =~ $version_pattern ]]; then
  printf '::error::Invalid release tag %q. Use vMAJOR.MINOR.PATCH with an optional prerelease suffix containing only letters, digits, dots, or hyphens.\n' "$tag" >&2
  exit 1
fi

version=${tag#v}
IFS=. read -r -a components <<< "${version%%-*}"
for component in "${components[@]}"; do
  # Remove leading zeros before checking length or interpreting decimal digits.
  component=${component#"${component%%[!0]*}"}
  component=${component:-0}
  if (( ${#component} > 5 )) || (( 10#$component > 65535 )); then
    printf '::error::Invalid release tag %q. Each numeric version component must be between 0 and 65535.\n' "$tag" >&2
    exit 1
  fi
done

printf '%s\n' "$version"
