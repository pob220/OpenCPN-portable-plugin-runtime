#!/usr/bin/env bash
set -euo pipefail

readonly baseline="${1:-Release_5.14.0}"
readonly allowed_repository_files='^(\.gitignore|\.gitmodules|\.github/workflows/.*)$'

git rev-parse --verify "${baseline}^{commit}" >/dev/null

failed=0
while IFS=$'\t' read -r record path; do
  [[ -n "${path}" ]] || continue
  if [[ "${path}" =~ ${allowed_repository_files} ]]; then
    continue
  fi
  read -r mode type expected <<<"${record}"
  if [[ "${type}" != "blob" ]]; then
    continue
  fi
  if [[ "${mode}" == "120000" ]]; then
    if [[ ! -L "${path}" ]]; then
      printf 'stock OpenCPN symlink is missing from %s: %s\n' "${baseline}" "${path}" >&2
      failed=1
      continue
    fi
    actual=$(printf '%s' "$(readlink -- "${path}")" | git hash-object --stdin)
  elif [[ ! -f "${path}" ]]; then
    printf 'stock OpenCPN file is missing from %s: %s\n' "${baseline}" "${path}" >&2
    failed=1
    continue
  else
    actual=$(git hash-object --path="${path}" "${path}")
  fi
  if [[ "${actual}" != "${expected}" ]]; then
    printf 'stock OpenCPN file differs from %s: %s\n' "${baseline}" "${path}" >&2
    failed=1
  fi
done < <(git ls-tree -r "${baseline}")

if ((failed)); then
  exit 1
fi

printf 'stock OpenCPN invariant passed against %s\n' "${baseline}"
