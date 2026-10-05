#!/usr/bin/env bash
set -euo pipefail

tag=${1:-}
version_pattern='^v[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9]+([.-][A-Za-z0-9]+)*)?$'

if [[ ! "$tag" =~ $version_pattern ]]; then
  printf '::error::Invalid release tag %q. Use vMAJOR.MINOR.PATCH with an optional prerelease suffix containing only letters, digits, dots, or hyphens.\n' "$tag" >&2
  exit 1
fi

printf '%s\n' "${tag#v}"
