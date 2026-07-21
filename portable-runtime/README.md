# Experimental OpenCPN portable-plugin runtime

This directory implements the hybrid Component Model runtime, standalone
iGRIB environmental provider and iWeatherRouting consumer for OpenCPN 5.14.0.
It is compiled only with
`OCPN_ENABLE_PORTABLE_PLUGINS=ON`; both the build flag and independent runtime
setting default off. The existing native loader is unchanged.

The proof embeds Wasmtime, negotiates a typed WIT world and provides actions,
position values, namespaced settings/private storage, host HTTP, cancellable
jobs, batched chart coverage, retained overlays and a host-rendered
environmental UI. iWeatherRouting runs its adaptive time-layer search in Wasm
and consumes time-indexed iGRIB batches plus host chart checks through typed
interfaces. Parallel departure comparisons use up to four isolated Wasmtime
Stores sharing one compiled component and capability set. Signed target
helpers perform bounded ecCodes decode and
environmental generation outside OpenCPN. Neither plugin depends on a native
xGRIB or Weather Routing plugin. Current evidence and remaining gates are in
`docs/portable-plugin-runtime/prototype-status.md`.

The environmental surface is declarative UI schema 2. Its package owns the
thirteen field-group catalogue, pressure levels, provider choices,
requirements, generator policy and persisted controller state. OpenCPN owns a
generic environmental dataset/view/render service: it has no provider IDs or
iGRIB-specific dialog class. `environment.provider@0.1` is negotiated using a
real semantic-version range, and iWeatherRouting holds a private immutable
dataset snapshot identified by size and SHA-256 for the lifetime of a route
job.

iWeatherRouting accepts real OpenCPN weather-routing `.pol` files and boat
`.xml` manifests. OpenCPN parses and validates the user-selected file, then
sends bounded value-only polar grids to the Wasm component for TWS/TWA
interpolation. A conservative Nicholson 35 Mk1 polar is bundled as the initial
test model; selecting another model is remembered in the isolated profile.
The selected routing result owns its retained isochrones, cursor-inspection
traces, forecast-time boat marker and GPX export. Variable-departure runs open
a comparison table with passage and environmental metrics; selecting a row
changes all of those views together.

## Build and test

Use a Rust toolchain with `wasm32-wasip2`, ecCodes and jsoncpp. The optional
generator path points to a locally built `environmental-grib` helper. Populate
the locked Cargo cache once while online; CMake builds it offline thereafter.

```sh
rustup target add wasm32-wasip2
cargo fetch --locked --manifest-path portable-runtime/bridge/Cargo.toml
cargo fetch --locked --manifest-path portable-plugins/igrib/component/Cargo.toml
cargo fetch --locked --manifest-path \
  portable-plugins/iweather-routing/component/Cargo.toml
cmake -S . -B build-portable \
  -DOCPN_ENABLE_PORTABLE_PLUGINS=ON \
  -DOCPN_BUILD_TEST=ON \
  -DOCPN_IGRIB_GENERATOR_HELPER=/path/to/environmental-grib
cmake --build build-portable --parallel
ctest --test-dir build-portable --output-on-failure -R '^portable_'
```

The deterministic signed development packages are written below
`build-portable/portable-runtime/packages/` as
`org.opencpn.igrib-0.1.0.ocpnp` and
`org.opencpn.iweather-routing-0.1.0.ocpnp`.

## Development install/update

The checked-in key is test material, not a production publisher key:

```sh
python3 portable-runtime/tools/install_package.py \
  --root /path/to/test-profile/portable-plugins \
  --trusted-key \
org.opencpn.development.igrib-2026=portable-runtime/development-keys/igrib-ed25519-public.pem \
  --developer --replace \
  build-portable/portable-runtime/packages/org.opencpn.igrib-0.1.0.ocpnp

python3 portable-runtime/tools/install_package.py \
  --root /path/to/test-profile/portable-plugins \
  --trusted-key \
org.opencpn.development.portable-reference-2026=portable-runtime/development-keys/igrib-ed25519-public.pem \
  --developer --replace \
  build-portable/portable-runtime/packages/org.opencpn.iweather-routing-0.1.0.ocpnp
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

python3 portable-runtime/tests/routing_package_conformance.py \
  --package build-portable/portable-runtime/packages/org.opencpn.iweather-routing-0.1.0.ocpnp \
  --trusted-key portable-runtime/development-keys/igrib-ed25519-public.pem
```

Do not enable this prototype in a navigation-critical profile. Plugin weather,
overlays, chart-coverage observations and generated GRIBs are experimental
planning data, not authoritative safety information.

The `Portable runtime conformance` GitHub workflow builds target-qualified
helpers, assembles one developer-signed package, then runs it on Windows,
both macOS architectures, both native Linux architectures and inside actual
Flatpak SDK sandboxes. Its aggregation tool refuses differing portable
components, conflicting helpers, undeclared targets or an incomplete required
matrix:

```sh
python3 portable-runtime/tools/assemble_multitarget_package.py --help
```
