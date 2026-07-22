# Prototype results

This is a cumulative experiment log. Prototypes are isolated, reproducible and
excluded from normal OpenCPN release targets.

## P1 — multi-package toolbar actions

Location: `experiments/runtime-host-toolbar-plugin`

Host under test: unmodified OpenCPN 5.14.0 at
`91f3b674366068a6ecd61a5e9aba204bba85f57e`, built from a temporary `git
archive` source snapshot. GCC 16 required demoting two warnings which 5.14
promotes to errors; no source change was made to the stock application.

Build and registry test:

```sh
cmake -S experiments/runtime-host-toolbar-plugin \
  -B build/runtime-host-toolbar-probe -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/runtime-host-toolbar-probe --parallel
ctest --test-dir build/runtime-host-toolbar-probe --output-on-failure
```

Result: pass, 1/1 tests. The test covers duplicate logical keys, duplicate
numeric IDs, forward/reverse lookup, removal, and re-registration of the same
logical key under a new native ID.

Stock host build:

```sh
git archive --format=tar -o /tmp/opencpn-stock-514.tar Release_5.14.0
cmake -S /tmp/opencpn-stock-514-src -B /tmp/opencpn-stock-514-build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  '-DCMAKE_CXX_FLAGS:STRING=-Wno-error=array-bounds -Wno-alloc-size-larger-than'
cmake --build /tmp/opencpn-stock-514-build --target opencpn --parallel 6
```

Runtime sequence used a self-contained portable tree below `/tmp` and
`OCPN_RUNTIME_HOST_PROBE_AUTORUN=1`. The stock log recorded successful late
add, duplicate rejection, independent check state, dynamic removal,
re-registration, trap cleanup, recovery, DeInit cleanup and clean application
exit. Numeric IDs changed from 1575 to 1577 for reloaded iGRIB and from 1576 to
1578 for recovered iWeatherRouting while logical keys remained stable.

What this proves:

- one managed-plugin-shaped native module can register three child icons;
- multiple IDs route through one owner and can be resolved safely;
- late add/remove works without restart using only public API;
- stock insertion needs an explicit rebuild trigger;
- checked state is independently mutable;
- a failed child need not leave a stale icon;
- shutdown can remove all actions cleanly.

What it does not prove:

- native enabled/disabled state, which is absent from API 1.21;
- user customisation/placement persistence;
- pixel-perfect behaviour on every theme, DPI and platform;
- Plugin Manager identity for child packages;
- actual Wasmtime execution or the other host services.

Incidental defect found and fixed in the probe: `GetPlugInBitmap()` must return
a valid object because stock 5.14 dereferences it unconditionally. The failing
address resolved to `model/src/plugin_loader.cpp:668`.

## P2 — plugin-shaped Wasmtime lifecycle

The optional experiment target linked the existing clean-HEAD Rust bridge into
the same conventional module. Inputs were built from a `git archive` at the
study base so concurrent working-tree changes were excluded:

```sh
CARGO_HOME=.portable-toolchain/cargo \
RUSTUP_HOME=.portable-toolchain/rustup \
CARGO_TARGET_DIR=/tmp/runtime-host-study-cargo-target \
cargo build --locked --offline --release \
  --manifest-path /tmp/runtime-host-study-head/portable-runtime/bridge/Cargo.toml

cargo build --locked --offline --release --target wasm32-wasip2 \
  --manifest-path /tmp/runtime-host-study-head/portable-plugins/igrib/component/Cargo.toml
cargo build --locked --offline --release --target wasm32-wasip2 \
  --manifest-path /tmp/runtime-host-study-head/portable-plugins/iweather-routing/component/Cargo.toml

cmake -S experiments/runtime-host-toolbar-plugin \
  -B build/runtime-host-toolbar-wasmtime-probe \
  -DRUNTIME_HOST_PROBE_BRIDGE_LIBRARY=/tmp/runtime-host-study-cargo-target/release/libocpn_portable_runtime_bridge.a \
  -DRUNTIME_HOST_PROBE_BRIDGE_INCLUDE_DIR=/tmp/runtime-host-study-head/portable-runtime/bridge/include
cmake --build build/runtime-host-toolbar-wasmtime-probe --parallel
```

The clean standalone `bridge_smoke.cpp` also passed against both components.
It covers normal lifecycle, identity mismatch, deliberate trap containment,
provider routing, concurrent compute replicas, cancellation, route mutation,
polar values and disable/destroy.

The module loaded into stock 5.14, instantiated iGRIB, called initialize and
enable, invoked `igrib.toggle`, received three guest actions, one five-point
scene, one job and one setting, then disabled and destroyed the runtime before
plugin cleanup. The bridge archive was 40 MiB and the linked module 22 MiB.

This proves that Wasmtime/component execution can principally live inside a
normal plugin. It does not prove that `panic=abort`, allocator faults or other
native failures are isolated from OpenCPN; they are not.

## P3 — retained overlay bridge

The plugin retained the geographic polyline delivered by the guest and
implemented only public API 1.21 callbacks:

- `RenderOverlayMultiCanvas` converted points with `GetCanvasPixLL` and drew a
  software polyline;
- `RenderGLOverlayMultiCanvas` consumed the same immutable points and drew a GL
  line strip.

Stock log evidence recorded:

```text
RUNTIME_HOST_OVERLAY event=software-render canvas=0 points=5
RUNTIME_HOST_OVERLAY event=gl-render canvas=0 points=5
```

The first GL attempt correctly fell back to software because the temporary
portable installation lacked its stock `opencpn-glutil`. After building and
installing that unmodified stock target into the temporary tree, OpenCPN
reported OpenGL capable and invoked the GL callback. This proves both render
paths and lifecycle cleanup, not pixel-perfect cross-theme/platform output.

## P4 — job, cancellation and shutdown

The iGRIB guest's `start-job` host call launched a bounded native worker. The
OpenCPN UI and GL callbacks continued while it ran. Terminating the isolated
stock test instance caused `DeInit` to cancel and join the worker before guest
disable/destroy. The recorded join latency was 30 ms and the worker stopped at
progress 110/600. The log's cross-thread message ordering placed the worker's
cancel text after the join text, but `std::thread::join` was complete before
runtime destruction.

Together with `bridge_smoke.cpp` fuel/epoch/cooperative cancellation cases,
this proves a viable job bridge and shutdown ordering. It does not make an
arbitrary blocking third-party native call cancellable; helpers need process
deadlines.

## P5 — public paths and navigation reach

On stock 5.14 the module called only exported functions and logged:

```text
RUNTIME_HOST_PUBLIC_API event=paths private-root=/tmp/opencpn-stock-514-install/bin installed-data=
RUNTIME_HOST_PUBLIC_API event=navigation routes=0 waypoints=0 chart-directories=0
```

The empty counts are expected in the fresh portable profile. Successful
link/load/call proves access to the writable OpenCPN-private root and public
route, waypoint and chart-directory enumeration without private headers.
`GetPluginDataDir` was empty because the experimental module was copied rather
than installed as a managed package; official documentation confirms that a
managed plugin's returned data directory is installer-owned/read-only.

Source and the production Weather Routing plugin additionally establish route
retrieval and creation. The experiment deliberately did not mutate navigation
objects in the empty stock profile.

## P6 — harmless helper supervision

Linux builds include `helper_supervisor_probe`. It uses `posix_spawn` with a
new process group, redirects a readiness pipe, creates one harmless child and
grandchild that only pause, sends group termination, waits for the direct
child and verifies the grandchild is stopped/zombie. Both normal and Wasmtime
build trees pass this CTest.

```sh
ctest --test-dir build/runtime-host-toolbar-probe --output-on-failure
# action_registry and helper_supervisor: 2/2 passed
```

This proves argument-vector launch, pipe setup, cancellation and process-tree
cleanup available to normal plugin code on this Linux host. It does not prove
bubblewrap availability, Flatpak nesting, Windows Job Objects or macOS sandbox
profiles; those remain per-target gates.

## P7 — child package build/install/update policy

The clean-HEAD package tool suite ran in a temporary directory:

```sh
python3 -m unittest -v package_tools_test.py
```

Result: 18/18 passed. Coverage includes deterministic build, Ed25519 signature
and tamper rejection, checksum tamper, traversal, symlink, executable-location,
entry-count, control-character, case-collision and reserved-path rejection,
target-package assembly, atomic replacement and retained rollback. It never
wrote into the native managed-plugin install tree.

The beta tool suite passed 12/12 when run with the pinned environmental
generator submodule present. Running it from a top-level `git archive` first
produced two expected fixture errors because archives do not contain submodule
content or its Git metadata; the missing content test passed after overlaying
the submodule, and the commit-integrity test passed in the pinned checkout.

This proves the existing installer is a strong extraction base. It does not
provide a production trust root, revocation, child catalogue governance or an
OpenCPN Plugin Manager entry.

## Combined stock-host command

The final interactive integration run used only the disposable tree:

```sh
OCPN_RUNTIME_HOST_PROBE_COMPONENT=/tmp/runtime-host-study-igrib-target/wasm32-wasip2/release/igrib_portable_component.wasm \
  /tmp/opencpn-stock-514-install/bin/opencpn -p
```

No plugin was copied to, and no configuration was read from or written to, the
user's separate OpenCPN 5.15 installation/profile.
