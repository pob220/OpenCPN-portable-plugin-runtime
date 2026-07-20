#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
build_root="${OCPN_BETA_BUILD_ROOT:-$repo_root/build-portable-beta}"
opencpn_build="${OCPN_BETA_OPENCPN_BUILD_DIR:-$build_root/opencpn}"
package="$opencpn_build/portable-runtime/packages/org.opencpn.igrib-0.1.0.ocpnp"
routing_package="$opencpn_build/portable-runtime/packages/org.opencpn.iweather-routing-0.1.0.ocpnp"
trusted_key="$repo_root/portable-runtime/development-keys/igrib-ed25519-public.pem"

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
  printf '%s\n' \
    "Usage: portable-runtime/beta/run-conformance.sh [--live-copernicus] [GRIB_FIXTURE]" \
    "" \
    "Without a fixture, run package/helper policy checks." \
    "With a fixture, also decode it and run the existing-file generator path." \
    "--live-copernicus runs a bounded authenticated NWS current download" \
    "using COPERNICUSMARINE_SERVICE_USERNAME and" \
    "COPERNICUSMARINE_SERVICE_PASSWORD."
  exit 0
fi
if [[ ! -f "$package" ]]; then
  printf 'Built iGRIB package not found: %s\n' "$package" >&2
  exit 1
fi
if [[ ! -f "$routing_package" ]]; then
  printf 'Built iWeatherRouting package not found: %s\n' \
    "$routing_package" >&2
  exit 1
fi

ctest --test-dir "$opencpn_build" --output-on-failure -R '^portable_'

arguments=(
  --package "$package"
  --trusted-key "$trusted_key"
)
fixture=
for argument in "$@"; do
  if [[ "$argument" == --live-copernicus ]]; then
    arguments+=(--live-copernicus)
  elif [[ -z "$fixture" ]]; then
    fixture="$argument"
  else
    printf 'Unexpected argument: %s\n' "$argument" >&2
    exit 2
  fi
done
if [[ -n "$fixture" ]]; then
  if [[ ! -f "$fixture" ]]; then
    printf 'GRIB fixture not found: %s\n' "$fixture" >&2
    exit 1
  fi
  arguments+=(--fixture "$fixture" --full-generator)
fi
python3 "$repo_root/portable-runtime/tests/conformance.py" "${arguments[@]}"
python3 "$repo_root/portable-runtime/tests/routing_package_conformance.py" \
  --package "$routing_package" --trusted-key "$trusted_key"
