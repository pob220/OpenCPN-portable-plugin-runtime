#!/usr/bin/env bash
set -euo pipefail

packages=(
  at-spi2-core
  bubblewrap
  build-essential
  ca-certificates
  cmake
  curl
  dbus
  dbus-x11
  gettext
  gir1.2-atspi-2.0
  git
  libarchive-dev
  libblosc-dev
  libbz2-dev
  libcurl4-openssl-dev
  libdrm-dev
  libeccodes-dev
  libelf-dev
  libexif-dev
  libgdk-pixbuf-2.0-dev
  libglew-dev
  libgl1-mesa-dev
  libglu1-mesa-dev
  libgtest-dev
  libgtk-3-dev
  libjsoncpp-dev
  libjs-highlight.js
  libjs-mathjax
  liblz4-dev
  liblzma-dev
  libnetcdf-dev
  libpango1.0-dev
  libproj-dev
  libqhull-dev
  libshp-dev
  libsndfile1-dev
  libsqlite3-dev
  libssl-dev
  libtinyxml-dev
  libudev-dev
  libunarr-dev
  libusb-1.0-0-dev
  libwxgtk3.2-dev
  libwxgtk-webview3.2-dev
  libwxsvg-dev
  libzip-dev
  lsb-release
  ninja-build
  pkg-config
  portaudio19-dev
  python3
  python3-cryptography
  python3-dbus
  python3-gi
  rapidjson-dev
  util-linux
)

usage() {
  printf '%s\n' \
    "Usage: $0 [--install]" \
    "" \
    "Without --install, print the Debian/Ubuntu package command." \
    "With --install, run apt-get update and install the reviewed list."
}

mode=print
case "${1:-}" in
  "") ;;
  --install) mode=install ;;
  --help|-h) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

if [[ ! -r /etc/debian_version ]]; then
  printf 'This helper supports Debian/Ubuntu apt hosts only.\n' >&2
  exit 1
fi

if [[ "$mode" == print ]]; then
  printf 'Review, then run:\n\n  sudo apt-get update\n  sudo apt-get install'
  printf ' %q' "${packages[@]}"
  printf '\n\nInstall current stable Rust separately using https://rust-lang.org/install.html\n'
  exit 0
fi

sudo apt-get update
sudo apt-get install "${packages[@]}"

printf '\nNative packages installed. Install current stable Rust next:\n'
printf "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh\n"
