# Experimental OpenCPN portable-plugin runtime

This directory contains the Component Model runtime, contracts, author SDK and
reference portable packages used by the **Portable Plugin Manager**. The
Manager is a conventional native OpenCPN plugin loaded by an otherwise stock
OpenCPN 5.14.0 build. Portable support is therefore opt-in without modifying
or replacing OpenCPN's native plugin loader.

The earlier core-integrated prototype is retained in repository history and
some historical test documents. The current runtime-host build and isolated
profile procedure is documented in `tools/runtime-host/README.md`.

The Manager embeds Wasmtime, negotiates versioned typed WIT worlds and
provides actions,
position values, namespaced settings/private storage, host HTTP, cancellable
jobs, batched chart coverage, retained overlays and a host-rendered
environmental UI. iWeatherRouting runs a bounded forward-isocrone,
reverse-isocrone-recovery and time-dependent graph cascade in Wasm and consumes
time-indexed iGRIB batches plus host chart checks through typed interfaces.
Stage-labelled progress keeps long recovery calculations observable and
cancellable. Parallel departure comparisons use up to four isolated Wasmtime
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
job. The routing form polls the generic provider summary while it is open, and
each new calculation acquires the latest completed iGRIB dataset before fixing
that immutable per-job snapshot.

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
tools/runtime-host/build-linux.sh
```

The runtime-host workflow verifies that the OpenCPN source base is stock,
builds the Manager and reference components, runs the conformance suites and
installs deterministic development packages into the isolated RuntimeHost
profile.

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
