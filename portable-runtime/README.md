# Experimental portable-plugin vertical slice

This directory implements the reviewed architecture's narrow Stage 2 proof for
OpenCPN 5.14.0. It is compiled only with
`OCPN_ENABLE_PORTABLE_PLUGINS=ON`; both the build flag and the independent
runtime setting default to off. The existing native loader is unchanged.

The proof embeds Wasmtime's Component Model, loads the Rust iGRIB component,
negotiates a typed WIT world, registers actions, reads position, persists a
setting, runs a cancellable host job, renders a retained geographic polyline
and contains a deliberate guest trap. iGRIB's exact xGRIB UI is supplied by a
temporary typed compatibility service to an enabled native xGRIB plugin. It is
not yet an independent portable GRIB decoder or a production package system;
see `docs/portable-plugin-runtime/prototype-status.md`.

## Build

Use a Rust toolchain with the `wasm32-wasip2` target. Populate the locked Cargo
cache once while online; CMake then deliberately builds offline.

```sh
rustup target add wasm32-wasip2
cargo fetch --locked --manifest-path portable-runtime/bridge/Cargo.toml
cargo fetch --locked --manifest-path portable-plugins/igrib/component/Cargo.toml
cmake -S . -B build-portable \
  -DOCPN_ENABLE_PORTABLE_PLUGINS=ON \
  -DOCPN_BUILD_TEST=ON
cmake --build build-portable --parallel
ctest --test-dir build-portable --output-on-failure \
  -R 'portable_runtime|portable_package'
```

The deterministic development package is written to
`build-portable/portable-runtime/packages/org.opencpn.igrib-0.1.0.ocpnp`.

## Development install

Unsigned packages are accepted only through the explicit developer path:

```sh
python3 portable-runtime/tools/install_package.py \
  build-portable/portable-runtime/packages/org.opencpn.igrib-0.1.0.ocpnp \
  --root /path/to/profile/portable-plugins \
  --developer
```

Then set these keys in that test profile only:

```ini
[PortablePlugins]
EnableExperimental=1
DeveloperMode=1
```

Do not enable this prototype in a navigation-critical or production profile.
Packages and outputs are untrusted experimental data, not authoritative
navigation, chart-safety or weather information.
