#!/usr/bin/env bash
set -euo pipefail

readonly script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly source_root="$(cd "${script_dir}/../.." && pwd)"
readonly runtime_root="${OCPN_RUNTIME_HOST_ROOT:-/home/paul/RuntimeHost-OpenCPN}"
readonly build_root="${PPM_REFERENCE_BUILD_ROOT:-${runtime_root}/build-reference-packages}"
readonly helper_build="${build_root}/helper-build"
readonly helper_stage="${build_root}/helper-stage"
readonly package_stage="${build_root}/stage"
readonly output_root="${build_root}/packages"
readonly cargo_root="${build_root}/cargo"
readonly signing_key="${PPM_DEVELOPMENT_SIGNING_KEY:-${source_root}/portable-runtime/development-keys/igrib-ed25519-private.pem}"
readonly public_key="${PPM_DEVELOPMENT_PUBLIC_KEY:-${source_root}/portable-runtime/development-keys/igrib-ed25519-public.pem}"
readonly jobs="${OCPN_RUNTIME_HOST_JOBS:-$(nproc)}"

usage() {
  cat <<EOF
Usage: tools/runtime-host/package-reference-plugins.sh

Builds current-contract Linux packages for iGRIB, iWeatherRouting and iPolars.
The deterministic signed archives are written below:

  ${output_root}

Environment overrides:
  OCPN_RUNTIME_HOST_ROOT        ${runtime_root}
  PPM_REFERENCE_BUILD_ROOT     ${build_root}
  PPM_DEVELOPMENT_SIGNING_KEY  ${signing_key}
  PPM_DEVELOPMENT_PUBLIC_KEY   ${public_key}
  OCPN_RUNTIME_HOST_JOBS       ${jobs}

If the default disposable development key pair is absent, it is generated.
The public key is copied beside the archives as development-trust.pem; install
that key under each package key id when testing in a different isolated store.
EOF
}

case "${1:-}" in
  "") ;;
  --help|-h) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

if [[ "$(uname -s)" != Linux ]]; then
  printf 'This reference build currently supports Linux only.\n' >&2
  exit 1
fi
case "$(uname -m)" in
  x86_64|amd64) readonly helper_target="linux-gnu-x86_64" ;;
  aarch64|arm64) readonly helper_target="linux-gnu-aarch64" ;;
  *)
    printf 'Unsupported Linux processor: %s\n' "$(uname -m)" >&2
    exit 1
    ;;
esac

for command_name in cmake cargo rustc rustup python3 pkg-config; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    printf 'Missing required command: %s\n' "${command_name}" >&2
    exit 1
  fi
done
python3 -c 'import cryptography' >/dev/null || {
  printf 'Python cryptography is required for package signing.\n' >&2
  exit 1
}
if ! rustup target list --installed | grep -Fxq wasm32-wasip2; then
  printf 'The Rust wasm32-wasip2 target is not installed.\n' >&2
  printf 'Run: rustup target add wasm32-wasip2\n' >&2
  exit 1
fi
if [[ ! -f "${signing_key}" || ! -f "${public_key}" ]]; then
  if [[ "${signing_key}" != "${source_root}/portable-runtime/development-keys/igrib-ed25519-private.pem" ||
        "${public_key}" != "${source_root}/portable-runtime/development-keys/igrib-ed25519-public.pem" ]]; then
    printf 'The configured development signing key pair does not exist.\n' >&2
    exit 1
  fi
  python3 \
    "${source_root}/portable-runtime/development-keys/generate-development-keys.py"
fi

python3 - "${signing_key}" "${public_key}" <<'PY'
import pathlib
import sys
from cryptography.hazmat.primitives import serialization

private = serialization.load_pem_private_key(
    pathlib.Path(sys.argv[1]).read_bytes(), password=None
)
expected = serialization.load_pem_public_key(pathlib.Path(sys.argv[2]).read_bytes())
derived = private.public_key().public_bytes(
    serialization.Encoding.Raw, serialization.PublicFormat.Raw
)
actual = expected.public_bytes(
    serialization.Encoding.Raw, serialization.PublicFormat.Raw
)
if derived != actual:
    raise SystemExit("The development private and public keys do not match")
PY

readonly generator_source="${source_root}/portable-runtime/vendor/environmental-grib-generator"
if [[ ! -f "${generator_source}/CMakeLists.txt" ]]; then
  printf 'The pinned environmental generator submodule is not populated.\n' >&2
  printf 'Run: git submodule update --init --recursive\n' >&2
  exit 1
fi

cmake -S "${source_root}/portable-runtime/helper" -B "${helper_build}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="${helper_stage}" \
  -DOCPN_PORTABLE_HELPER_TARGET="${helper_target}" \
  -DBUILD_TESTING=ON
cmake --build "${helper_build}" --parallel "${jobs}"
ctest --test-dir "${helper_build}" --output-on-failure
cmake --install "${helper_build}"

build_component() {
  local package_id="$1"
  local crate="$2"
  local output_name="$3"
  local target_root="${cargo_root}/${package_id}"
  CARGO_TARGET_DIR="${target_root}" cargo build --locked --offline --release \
    --target wasm32-wasip2 --manifest-path "${crate}/Cargo.toml"
  printf '%s\n' "${target_root}/wasm32-wasip2/release/${output_name}"
}

readonly igrib_component="$(build_component \
  igrib "${source_root}/portable-plugins/igrib/component" \
  igrib_portable_component.wasm)"
readonly iweather_component="$(build_component \
  iweather-routing "${source_root}/portable-plugins/iweather-routing/component" \
  iweather_routing_portable_component.wasm)"
readonly ipolars_component="$(build_component \
  ipolars "${source_root}/portable-plugins/ipolars/component" \
  ipolars_portable_component.wasm)"

reset_package() {
  local root="$1"
  case "${root}" in
    "${package_stage}/"*) ;;
    *)
      printf 'Refusing to reset package outside the staging root: %s\n' \
        "${root}" >&2
      exit 1
      ;;
  esac
  cmake -E remove_directory "${root}"
  cmake -E make_directory "${root}"
}

copy_file() {
  local source="$1"
  local destination="$2"
  cmake -E make_directory "$(dirname "${destination}")"
  cmake -E copy_if_different "${source}" "${destination}"
}

readonly igrib_root="${package_stage}/igrib"
reset_package "${igrib_root}"
copy_file "${igrib_component}" "${igrib_root}/component/igrib.wasm"
copy_file "${source_root}/portable-plugins/igrib/package/manifest.json" \
  "${igrib_root}/manifest.json"
copy_file "${source_root}/portable-plugins/igrib/package/igrib.svg" \
  "${igrib_root}/resources/igrib.svg"
for control in prev next play stop now openfile setting curdata request ncurdata; do
  copy_file "${source_root}/plugins/grib_pi/data/${control}.svg" \
    "${igrib_root}/resources/controls/${control}.svg"
done
copy_file "${source_root}/data/svg/markicons/Hazard-Warning.svg" \
  "${igrib_root}/resources/fault-test.svg"
copy_file "${source_root}/data/svg/traditional/emblem-download.svg" \
  "${igrib_root}/resources/http-download.svg"
copy_file "${source_root}/portable-plugins/igrib/package/igrib-viewer.ui.json" \
  "${igrib_root}/ui/igrib-viewer.ui.json"
cmake -E copy_directory "${source_root}/portable-runtime/contracts/0.5" \
  "${igrib_root}/interfaces/opencpn-opp-0.5"
copy_file "${source_root}/COPYING.gplv2" \
  "${igrib_root}/licenses/GPL-2.0.txt"
copy_file "${source_root}/portable-plugins/igrib/README.md" \
  "${igrib_root}/README.md"
cmake -E copy_directory "${helper_stage}/helpers" "${igrib_root}/helpers"

readonly iweather_root="${package_stage}/iweather-routing"
reset_package "${iweather_root}"
copy_file "${iweather_component}" \
  "${iweather_root}/component/iweather-routing.wasm"
copy_file \
  "${source_root}/portable-plugins/iweather-routing/package/manifest.json" \
  "${iweather_root}/manifest.json"
copy_file \
  "${source_root}/portable-plugins/iweather-routing/package/iweather-routing.svg" \
  "${iweather_root}/resources/iweather-routing.svg"
copy_file \
  "${source_root}/portable-plugins/iweather-routing/package/Nicholson35_Mk1_cruising_realistic.pol" \
  "${iweather_root}/resources/Nicholson35_Mk1_cruising_realistic.pol"
copy_file \
  "${source_root}/portable-plugins/iweather-routing/package/iweather-routing.ui.json" \
  "${iweather_root}/ui/iweather-routing.ui.json"
cmake -E copy_directory "${source_root}/portable-runtime/contracts/0.5" \
  "${iweather_root}/interfaces/opencpn-opp-0.5"
copy_file "${source_root}/COPYING.gplv2" \
  "${iweather_root}/licenses/GPL-2.0.txt"
copy_file "${source_root}/portable-plugins/iweather-routing/README.md" \
  "${iweather_root}/README.md"

readonly ipolars_root="${package_stage}/ipolars"
reset_package "${ipolars_root}"
copy_file "${ipolars_component}" "${ipolars_root}/component/ipolars.wasm"
copy_file "${source_root}/portable-plugins/ipolars/package/manifest.json" \
  "${ipolars_root}/manifest.json"
copy_file \
  "${source_root}/portable-plugins/ipolars/package/resources/ipolars.svg" \
  "${ipolars_root}/resources/ipolars.svg"
copy_file "${source_root}/portable-plugins/ipolars/package/ui/ipolars.ui.json" \
  "${ipolars_root}/ui/ipolars.ui.json"
cmake -E copy_directory "${source_root}/portable-runtime/contracts/0.4" \
  "${ipolars_root}/interfaces/opencpn-opp-0.4"
copy_file "${source_root}/COPYING.gplv2" \
  "${ipolars_root}/licenses/GPL-2.0.txt"
copy_file "${source_root}/portable-plugins/ipolars/README.md" \
  "${ipolars_root}/README.md"

manifest_version() {
  python3 -c \
    'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["version"])' \
    "$1"
}

mkdir -p "${output_root}"
remove_stale_archives() {
  local package_id="$1"
  local stale
  while IFS= read -r -d '' stale; do
    case "${stale}" in
      "${output_root}/${package_id}-"*.ocpnp)
        cmake -E remove "${stale}"
        ;;
      *)
        printf 'Refusing to remove an unexpected archive path: %s\n' \
          "${stale}" >&2
        exit 1
        ;;
    esac
  done < <(find "${output_root}" -maxdepth 1 -type f \
             -name "${package_id}-*.ocpnp" -print0)
}
remove_stale_archives org.opencpn.igrib
remove_stale_archives org.opencpn.iweather-routing
remove_stale_archives org.opencpn.ipolars
readonly igrib_archive="${output_root}/org.opencpn.igrib-$(manifest_version "${igrib_root}/manifest.json").ocpnp"
readonly iweather_archive="${output_root}/org.opencpn.iweather-routing-$(manifest_version "${iweather_root}/manifest.json").ocpnp"
readonly ipolars_archive="${output_root}/org.opencpn.ipolars-$(manifest_version "${ipolars_root}/manifest.json").ocpnp"
readonly packager="${source_root}/portable-runtime/tools/build_package.py"

python3 "${packager}" --root "${igrib_root}" --output "${igrib_archive}" \
  --signing-key "${signing_key}" \
  --key-id org.opencpn.development.igrib-2026
python3 "${packager}" --root "${iweather_root}" \
  --output "${iweather_archive}" --signing-key "${signing_key}" \
  --key-id org.opencpn.development.portable-reference-2026
python3 "${packager}" --root "${ipolars_root}" \
  --output "${ipolars_archive}" --signing-key "${signing_key}" \
  --key-id org.opencpn.development.portable-reference-2026
copy_file "${public_key}" "${output_root}/development-trust.pem"
copy_file "${public_key}" \
  "${output_root}/trust/org.opencpn.development.igrib-2026.pem"
copy_file "${public_key}" \
  "${output_root}/trust/org.opencpn.development.portable-reference-2026.pem"

sha256sum "${igrib_archive}" "${iweather_archive}" "${ipolars_archive}" \
  >"${output_root}/SHA256SUMS"

# Validate the completed archives, not merely their staging directories.  This
# catches signature, manifest, UI, helper-target and archive-shape regressions
# in every clean reference build.  A workstation with the optional real GRIB
# fixture can request the slower decode/generator path as well.
igrib_conformance=(
  python3 "${source_root}/portable-runtime/tests/conformance.py"
  --package "${igrib_archive}"
  --trusted-key "${public_key}"
  --output-json "${build_root}/igrib-package-conformance.json"
)
if [[ -n "${PPM_CONFORMANCE_GRIB_FIXTURE:-}" ]]; then
  if [[ ! -f "${PPM_CONFORMANCE_GRIB_FIXTURE}" ]]; then
    printf 'PPM_CONFORMANCE_GRIB_FIXTURE does not exist: %s\n' \
      "${PPM_CONFORMANCE_GRIB_FIXTURE}" >&2
    exit 1
  fi
  igrib_conformance+=(
    --fixture "${PPM_CONFORMANCE_GRIB_FIXTURE}"
    --full-generator
  )
fi
"${igrib_conformance[@]}"
python3 \
  "${source_root}/portable-runtime/tests/routing_package_conformance.py" \
  --package "${iweather_archive}" --trusted-key "${public_key}"
python3 \
  "${source_root}/portable-runtime/tests/ipolars_package_conformance.py" \
  --package "${ipolars_archive}" --trusted-key "${public_key}"

printf '\nCurrent-contract reference packages are ready:\n'
printf '  %s\n' "${igrib_archive}" "${iweather_archive}" "${ipolars_archive}"
printf 'Checksums: %s\n' "${output_root}/SHA256SUMS"
printf 'Development trust directory: %s\n' "${output_root}/trust"
