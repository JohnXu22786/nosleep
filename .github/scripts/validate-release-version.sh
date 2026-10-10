#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

tag=${1:-}
version_pattern='^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[A-Za-z0-9]+([.-][A-Za-z0-9]+)*)?$'

if [[ ! "$tag" =~ $version_pattern ]]; then
  printf '::error::Invalid release tag %q. Use vMAJOR.MINOR.PATCH with an optional prerelease suffix containing only letters, digits, dots, or hyphens.\n' "$tag" >&2
  exit 1
fi

if (( ${#tag} >= 64 )); then
  printf '::error::Invalid release tag %q. Tag names must be shorter than 64 bytes to fit the updater.\n' "$tag" >&2
  exit 1
fi

version=${tag#v}
if [[ "$version" == *-* ]]; then
  prerelease=${version#*-}
  IFS=. read -r -a identifiers <<< "$prerelease"
  for identifier in "${identifiers[@]}"; do
    if [[ "$identifier" =~ ^[0-9]+$ && ${#identifier} -gt 1 && "$identifier" == 0* ]]; then
      printf '::error::Invalid release tag %q. Numeric prerelease identifiers cannot have leading zeros.\n' "$tag" >&2
      exit 1
    fi
  done
fi

IFS=. read -r -a components <<< "${version%%-*}"
for component in "${components[@]}"; do
  if (( ${#component} > 5 )) || (( 10#$component > 65535 )); then
    printf '::error::Invalid release tag %q. Each numeric version component must be between 0 and 65535.\n' "$tag" >&2
    exit 1
  fi
done

printf '%s\n' "$version"
