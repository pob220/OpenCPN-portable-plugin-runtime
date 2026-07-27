#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
build_root="${OCPN_BETA_BUILD_ROOT:-$repo_root/build-portable-beta}"
opencpn_build="${OCPN_BETA_OPENCPN_BUILD_DIR:-$build_root/opencpn}"
generator_build="${OCPN_BETA_GENERATOR_BUILD_DIR:-$build_root/environmental-generator}"
stage_root="${OCPN_BETA_STAGE_ROOT:-$build_root/stage}"
jobs="${OCPN_BETA_JOBS:-$(nproc)}"
cmake_generator="${OCPN_BETA_CMAKE_GENERATOR:-Ninja}"
generator_source="$repo_root/portable-runtime/vendor/environmental-grib-generator"
generator_binary="$generator_build/environmental-grib"
igrib_package="$opencpn_build/portable-runtime/packages/org.opencpn.igrib-0.2.0.ocpnp"
iwr_package="$opencpn_build/portable-runtime/packages/org.opencpn.iweather-routing-0.2.0.ocpnp"
trusted_key="$repo_root/portable-runtime/development-keys/igrib-ed25519-public.pem"

usage() {
  cat <<EOF
Usage: portable-runtime/beta/build-linux.sh

Builds, tests and stages the complete Linux beta without system installation.

Environment overrides:
  OCPN_BETA_BUILD_ROOT             $build_root
  OCPN_BETA_OPENCPN_BUILD_DIR      $opencpn_build
  OCPN_BETA_GENERATOR_BUILD_DIR    $generator_build
  OCPN_BETA_STAGE_ROOT             $stage_root
  OCPN_BETA_CMAKE_GENERATOR        $cmake_generator
  OCPN_BETA_JOBS                   $jobs
  OCPN_BETA_SKIP_FETCH=1           require an already populated Cargo cache
EOF
}

case "${1:-}" in
  "") ;;
  --help|-h) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

if [[ "$(uname -s)" != Linux ]]; then
  printf 'This beta workflow currently supports Linux only.\n' >&2
  exit 1
fi
case "$(uname -m)" in
  x86_64|aarch64|arm64) ;;
  *) printf 'Unsupported beta processor: %s\n' "$(uname -m)" >&2; exit 1 ;;
esac

required_commands=(cmake cargo rustc rustup python3 pkg-config git bwrap prlimit)
for command_name in "${required_commands[@]}"; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'Missing required command: %s\n' "$command_name" >&2
    exit 1
  fi
done

python3 -c 'import cryptography' || {
  printf 'Python cryptography is required for package signing.\n' >&2
  exit 1
}
if [[ ! -f "$trusted_key" ]]; then
  python3 \
    "$repo_root/portable-runtime/development-keys/generate-development-keys.py"
fi

for module in eccodes jsoncpp netcdf libcurl qhull_r blosc libzip; do
  if ! pkg-config --exists "$module"; then
    printf 'Missing pkg-config development module: %s\n' "$module" >&2
    exit 1
  fi
done

if [[ ! -f "$generator_source/CMakeLists.txt" ]]; then
  printf 'The vendored environmental generator source is missing.\n' >&2
  exit 1
fi

rust_version="$(rustc --version | awk '{print $2}')"
rust_major="${rust_version%%.*}"
rust_remainder="${rust_version#*.}"
rust_minor="${rust_remainder%%.*}"
if [[ ! "$rust_major" =~ ^[0-9]+$ || ! "$rust_minor" =~ ^[0-9]+$ ]] || \
   (( rust_major < 1 || (rust_major == 1 && rust_minor < 94) )); then
  printf 'Rust 1.94 or later is required; found %s\n' "$(rustc --version)" >&2
  exit 1
fi

if [[ "${OCPN_BETA_SKIP_FETCH:-0}" != 1 ]]; then
  rustup target add wasm32-wasip2
  cargo fetch --locked --manifest-path "$repo_root/portable-runtime/bridge/Cargo.toml"
  cargo fetch --locked --manifest-path \
    "$repo_root/portable-plugins/igrib/component/Cargo.toml"
  cargo fetch --locked --manifest-path \
    "$repo_root/portable-plugins/iweather-routing/component/Cargo.toml"
elif ! rustup target list --installed | grep -Fxq wasm32-wasip2; then
  printf 'OCPN_BETA_SKIP_FETCH=1 but wasm32-wasip2 is not installed.\n' >&2
  exit 1
fi

cmake -S "$generator_source" -B "$generator_build" -G "$cmake_generator" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON
cmake --build "$generator_build" --parallel "$jobs"
ctest --test-dir "$generator_build" --output-on-failure

cmake -S "$repo_root" -B "$opencpn_build" -G "$cmake_generator" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$stage_root/app" \
  -DOCPN_ENABLE_PORTABLE_PLUGINS=ON \
  -DOCPN_BUILD_TEST=ON \
  -DOCPN_IGRIB_BUILD_NATIVE_HELPER=ON \
  -DOCPN_IGRIB_GENERATOR_HELPER="$generator_binary" \
  -DOCPN_PORTABLE_CARGO="$(command -v cargo)"
cmake --build "$opencpn_build" --parallel "$jobs"
ctest --test-dir "$opencpn_build" --output-on-failure -R '^portable_'
cmake --install "$opencpn_build" --prefix "$stage_root/app"

# OpenCPN portable mode intentionally resolves immutable runtime data beside
# the executable. Keep the staged install conventional, then add only missing
# relative links into bin; existing files are never replaced.
for source in "$stage_root/app/share/opencpn/"*; do
  [[ -e "$source" ]] || continue
  name="${source##*/}"
  destination="$stage_root/app/bin/$name"
  if [[ ! -e "$destination" && ! -L "$destination" ]]; then
    ln -s "../share/opencpn/$name" "$destination"
  fi
done
if [[ ! -e "$stage_root/app/bin/share" && \
      ! -L "$stage_root/app/bin/share" ]]; then
  ln -s ../share "$stage_root/app/bin/share"
fi

python3 "$repo_root/portable-runtime/tools/install_package.py" \
  "$igrib_package" \
  --root "$stage_root/config/portable-plugins" \
  --trusted-key \
  "org.opencpn.development.igrib-2026=$trusted_key" \
  --developer --replace
python3 "$repo_root/portable-runtime/tools/install_package.py" \
  "$iwr_package" \
  --root "$stage_root/config/portable-plugins" \
  --trusted-key \
  "org.opencpn.development.portable-reference-2026=$trusted_key" \
  --developer --replace
python3 "$script_dir/configure_profile.py" "$stage_root/config/opencpn.conf"

mkdir -p "$stage_root/home" "$stage_root/xdg-config" "$stage_root/xdg-data" \
  "$stage_root/xdg-cache"
{
  printf 'source_commit=%s\n' "$(git -C "$repo_root" rev-parse HEAD)"
  if [[ -n "$(git -C "$repo_root" status --short)" ]]; then
    printf 'source_dirty=yes\n'
  else
    printf 'source_dirty=no\n'
  fi
  printf 'generator_source=vendored\n'
  printf 'opencpn_sha256=%s\n' "$(sha256sum "$stage_root/app/bin/opencpn" | awk '{print $1}')"
  printf 'igrib_package_sha256=%s\n' "$(sha256sum "$igrib_package" | awk '{print $1}')"
  printf 'iweather_routing_package_sha256=%s\n' "$(sha256sum "$iwr_package" | awk '{print $1}')"
  printf 'rustc=%s\n' "$(rustc --version)"
  printf 'cmake=%s\n' "$(cmake --version | head -n 1)"
} >"$stage_root/BUILD-IDENTITY.txt"

printf '\nBeta build, tests and isolated installation completed.\n'
printf 'Stage: %s\n' "$stage_root"
printf 'Launch: %s/launch-linux.sh\n' "$script_dir"
printf 'Identity: %s/BUILD-IDENTITY.txt\n' "$stage_root"
