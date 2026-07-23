#!/usr/bin/env bash
set -euo pipefail

readonly script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly source_root="$(cd "${script_dir}/../.." && pwd)"
readonly runtime_root="${OCPN_RUNTIME_HOST_ROOT:-/home/paul/RuntimeHost-OpenCPN}"
readonly stock_build="${runtime_root}/build-stock"
readonly plugin_build="${runtime_root}/build-ppm"
readonly application_prefix="${runtime_root}/app"
readonly config_root="${runtime_root}/config"
readonly jobs="${OCPN_RUNTIME_HOST_JOBS:-$(nproc)}"

"${source_root}/tools/verify-stock-core.sh"

cmake -S "${source_root}" -B "${stock_build}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="${application_prefix}" \
  -DOCPN_BUILD_TEST=ON \
  "-DCMAKE_CXX_FLAGS=-Wno-error=alloc-size-larger-than= -Wno-error=array-bounds -Wno-error=stringop-overread"
cmake --build "${stock_build}" --parallel "${jobs}"
cmake --install "${stock_build}"

"${script_dir}/package-reference-plugins.sh"
readonly development_trust="${runtime_root}/build-reference-packages/packages/development-trust.pem"

cmake -S "${source_root}/plugins/portable_plugin_manager_pi" \
  -B "${plugin_build}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOPENCPN_SOURCE_DIR="${source_root}" \
  -DPPM_PLUGIN_LIBRARY_DIR="${config_root}/plugins/lib" \
  -DPPM_PLUGIN_DATA_DIR="${config_root}/share/opencpn/plugins/portable_plugin_manager_pi" \
  -DPPM_DEVELOPMENT_TRUST_KEY="${development_trust}"
cmake --build "${plugin_build}" --parallel "${jobs}"
ctest --test-dir "${plugin_build}" --output-on-failure
cmake --install "${plugin_build}"

python3 "${script_dir}/configure-profile.py" \
  "${config_root}/opencpn.conf"
mkdir -p "${config_root}/portable-runtime" \
  "${runtime_root}/xdg-config" "${runtime_root}/xdg-data" \
  "${runtime_root}/xdg-cache"
"${script_dir}/install-reference-packages.sh"

printf 'Runtime-host stock build and isolated installation are ready at %s\n' \
  "${runtime_root}"
