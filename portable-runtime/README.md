# Experimental OpenCPN portable-plugin runtime

This directory implements the hybrid Component Model runtime and standalone
iGRIB proof for OpenCPN 5.14.0. It is compiled only with
`OCPN_ENABLE_PORTABLE_PLUGINS=ON`; both the build flag and independent runtime
setting default off. The existing native loader is unchanged.

The proof embeds Wasmtime, negotiates a typed WIT world and provides actions,
position values, namespaced settings/private storage, host HTTP, cancellable
jobs, batched chart coverage, retained overlays and a host-rendered
environmental UI. Signed target helpers perform bounded ecCodes decode and
environmental generation outside OpenCPN. iGRIB does not depend on native
xGRIB. Current evidence and remaining gates are in
`docs/portable-plugin-runtime/prototype-status.md`.

## Build and test

Use a Rust toolchain with `wasm32-wasip2`, ecCodes and jsoncpp. The optional
generator path points to a locally built `environmental-grib` helper. Populate
the locked Cargo cache once while online; CMake builds it offline thereafter.

```sh
rustup target add wasm32-wasip2
cargo fetch --locked --manifest-path portable-runtime/bridge/Cargo.toml
cargo fetch --locked --manifest-path portable-plugins/igrib/component/Cargo.toml
cmake -S . -B build-portable \
  -DOCPN_ENABLE_PORTABLE_PLUGINS=ON \
  -DOCPN_BUILD_TEST=ON \
  -DOCPN_IGRIB_GENERATOR_HELPER=/path/to/environmental-grib
cmake --build build-portable --parallel
ctest --test-dir build-portable --output-on-failure -R '^portable_'
```

The deterministic signed development package is written to
`build-portable/portable-runtime/packages/org.opencpn.igrib-0.1.0.ocpnp`.

## Development install/update

The checked-in key is test material, not a production publisher key:

```sh
python3 portable-runtime/tools/install_package.py \
  --root /path/to/test-profile/portable-plugins \
  --trusted-key \
org.opencpn.development.igrib-2026=portable-runtime/development-keys/igrib-ed25519-public.pem \
  --developer --replace \
  build-portable/portable-runtime/packages/org.opencpn.igrib-0.1.0.ocpnp
```

`--replace` verifies and stages the new package, atomically switches it into
place and retains the previous package under `.rollback`.

Enable it only in a test profile:

```ini
[PortablePlugins]
EnableExperimental=1
DeveloperMode=1
```

## Target conformance

Run the same package-policy/helper checks on each target:

```sh
python3 portable-runtime/tests/conformance.py \
  --package build-portable/portable-runtime/packages/org.opencpn.igrib-0.1.0.ocpnp \
  --trusted-key portable-runtime/development-keys/igrib-ed25519-public.pem \
  --fixture /path/to/test.grb --full-generator
```

Do not enable this prototype in a navigation-critical profile. Plugin weather,
overlays, chart-coverage observations and generated GRIBs are experimental
planning data, not authoritative safety information.
