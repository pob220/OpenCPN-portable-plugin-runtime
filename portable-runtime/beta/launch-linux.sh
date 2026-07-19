#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
build_root="${OCPN_BETA_BUILD_ROOT:-$repo_root/build-portable-beta}"
stage_root="${OCPN_BETA_STAGE_ROOT:-$build_root/stage}"
executable="$stage_root/app/bin/opencpn"

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
  printf '%s\n' \
    "Usage: portable-runtime/beta/launch-linux.sh [-- OpenCPN-options]" \
    "" \
    "OCPN_BETA_STAGE_ROOT selects an alternate isolated stage."
  exit 0
fi
if [[ "${1:-}" == -- ]]; then
  shift
fi
if [[ ! -x "$executable" ]]; then
  printf 'Beta executable not found: %s\nRun build-linux.sh first.\n' \
    "$executable" >&2
  exit 1
fi

mkdir -p "$stage_root/config" "$stage_root/home" "$stage_root/xdg-config" \
  "$stage_root/xdg-data" "$stage_root/xdg-cache"
python3 "$script_dir/configure_profile.py" "$stage_root/config/opencpn.conf"

export HOME="$stage_root/home"
export XDG_CONFIG_HOME="$stage_root/xdg-config"
export XDG_DATA_HOME="$stage_root/xdg-data"
export XDG_CACHE_HOME="$stage_root/xdg-cache"
export OPENCPN_PLUGIN_DIRS="$stage_root/app/lib/opencpn"

unset WR_HEADLESS_ROUTE_TEST WR_HEADLESS_SCENARIO WR_HEADLESS_OUTPUT
unset DRI_PRIME __NV_PRIME_RENDER_OFFLOAD __VK_LAYER_NV_optimus
unset __GLX_VENDOR_LIBRARY_NAME VK_ICD_FILENAMES OPENCPN_COMPAT_TARGET

exec "$executable" --portable --configdir "$stage_root/config" --no_opengl "$@"
