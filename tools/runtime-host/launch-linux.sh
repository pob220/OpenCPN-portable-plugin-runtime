#!/usr/bin/env bash
set -euo pipefail

readonly runtime_root="${OCPN_RUNTIME_HOST_ROOT:-/home/paul/RuntimeHost-OpenCPN}"
readonly application="${runtime_root}/app/bin/opencpn"
readonly config_root="${runtime_root}/config"

if [[ ! -x "${application}" ]]; then
  printf 'Runtime-host OpenCPN is not built: %s\n' "${application}" >&2
  printf 'Run tools/runtime-host/build-linux.sh first.\n' >&2
  exit 1
fi

mkdir -p "${config_root}/portable-runtime" \
  "${runtime_root}/xdg-config" "${runtime_root}/xdg-data" \
  "${runtime_root}/xdg-cache"

export XDG_CONFIG_HOME="${runtime_root}/xdg-config"
export XDG_DATA_HOME="${runtime_root}/xdg-data"
export XDG_CACHE_HOME="${runtime_root}/xdg-cache"
export XDG_DATA_DIRS="${config_root}/share:${runtime_root}/app/share:/usr/local/share:/usr/share"
export OPENCPN_PLUGIN_DIRS="${config_root}/plugins/lib"
export OCPN_PORTABLE_PLUGIN_ROOT="${config_root}/portable-runtime"
export OCPN_PPM_DEVELOPER_MODE="${OCPN_PPM_DEVELOPER_MODE:-1}"

exec "${application}" -c "${config_root}" "$@"
