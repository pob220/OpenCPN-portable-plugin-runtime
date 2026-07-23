#!/usr/bin/env bash
set -euo pipefail

readonly script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly runtime_root="${OCPN_RUNTIME_HOST_ROOT:-/home/paul/RuntimeHost-OpenCPN}"
readonly package_root="${PPM_REFERENCE_PACKAGE_ROOT:-${runtime_root}/build-reference-packages/packages}"
readonly cli="${runtime_root}/build-ppm/ppm_package_cli"
readonly store="${runtime_root}/config/portable-runtime"
readonly trust="${runtime_root}/config/share/opencpn/plugins/portable_plugin_manager_pi/trust"

usage() {
  cat <<EOF
Usage: tools/runtime-host/install-reference-packages.sh

Transactionally installs, approves and enables the packages produced by
package-reference-plugins.sh in the isolated RuntimeHost profile.

Environment overrides:
  OCPN_RUNTIME_HOST_ROOT       ${runtime_root}
  PPM_REFERENCE_PACKAGE_ROOT  ${package_root}
EOF
}

case "${1:-}" in
  "") ;;
  --help|-h) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

if pgrep -af "${runtime_root}/app/bin/opencpn" >/dev/null; then
  printf 'The isolated RuntimeHost OpenCPN is running; close it first.\n' >&2
  exit 1
fi
if [[ ! -x "${cli}" ]]; then
  printf 'Package CLI is missing: %s\n' "${cli}" >&2
  exit 1
fi
if [[ ! -d "${trust}" ]]; then
  printf 'Manager trust directory is missing: %s\n' "${trust}" >&2
  exit 1
fi
for key_id in org.opencpn.development.igrib-2026 \
              org.opencpn.development.portable-reference-2026; do
  if ! cmp -s "${package_root}/development-trust.pem" \
      "${trust}/${key_id}.pem"; then
    printf 'Installed Manager trust does not match reference packages: %s\n' \
      "${key_id}" >&2
    printf 'Reconfigure the Manager with -DPPM_DEVELOPMENT_TRUST_KEY=%s\n' \
      "${package_root}/development-trust.pem" >&2
    exit 1
  fi
done

archive_for() {
  local package_id="$1"
  local -a matches=()
  while IFS= read -r -d '' path; do
    matches+=("${path}")
  done < <(find "${package_root}" -maxdepth 1 -type f \
            -name "${package_id}-*.ocpnp" -print0)
  if (( ${#matches[@]} != 1 )); then
    printf 'Expected one archive for %s; found %d\n' \
      "${package_id}" "${#matches[@]}" >&2
    return 1
  fi
  printf '%s\n' "${matches[0]}"
}

install_archive() {
  local archive="$1"
  local package_id="$2"
  "${cli}" --root "${store}" --trust "${trust}" --developer \
    inspect "${archive}"
  if "${cli}" --root "${store}" --trust "${trust}" --developer list |
      cut -f1 | grep -Fxq "${package_id}"; then
    "${cli}" --root "${store}" --trust "${trust}" --developer --replace \
      install "${archive}"
  else
    "${cli}" --root "${store}" --trust "${trust}" --developer \
      install "${archive}"
  fi
  "${cli}" --root "${store}" --trust "${trust}" --developer --yes \
    approve "${package_id}"
  "${cli}" --root "${store}" --trust "${trust}" --developer \
    state "${package_id}" on
  "${cli}" --root "${store}" --trust "${trust}" --developer \
    audit "${package_id}"
}

install_archive "$(archive_for org.opencpn.igrib)" org.opencpn.igrib
install_archive "$(archive_for org.opencpn.iweather-routing)" \
  org.opencpn.iweather-routing
install_archive "$(archive_for org.opencpn.ipolars)" org.opencpn.ipolars

printf '\nInstalled reference packages:\n'
"${cli}" --root "${store}" --trust "${trust}" --developer list
