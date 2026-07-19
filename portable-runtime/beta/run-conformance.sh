#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
build_root="${OCPN_BETA_BUILD_ROOT:-$repo_root/build-portable-beta}"
opencpn_build="${OCPN_BETA_OPENCPN_BUILD_DIR:-$build_root/opencpn}"
package="$opencpn_build/portable-runtime/packages/org.opencpn.igrib-0.1.0.ocpnp"
trusted_key="$repo_root/portable-runtime/development-keys/igrib-ed25519-public.pem"

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
  printf '%s\n' \
    "Usage: portable-runtime/beta/run-conformance.sh [GRIB_FIXTURE]" \
    "" \
    "Without a fixture, run package/helper policy checks." \
    "With a fixture, also decode it and run the existing-file generator path."
  exit 0
fi
if [[ ! -f "$package" ]]; then
  printf 'Built iGRIB package not found: %s\n' "$package" >&2
  exit 1
fi

ctest --test-dir "$opencpn_build" --output-on-failure -R '^portable_'

arguments=(
  --package "$package"
  --trusted-key "$trusted_key"
)
if [[ -n "${1:-}" ]]; then
  if [[ ! -f "$1" ]]; then
    printf 'GRIB fixture not found: %s\n' "$1" >&2
    exit 1
  fi
  arguments+=(--fixture "$1" --full-generator)
fi
python3 "$repo_root/portable-runtime/tests/conformance.py" "${arguments[@]}"
