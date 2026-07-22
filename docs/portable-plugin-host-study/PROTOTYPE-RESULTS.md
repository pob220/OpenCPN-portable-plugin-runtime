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

## Pending prototypes

- P2: plugin-shaped Wasmtime lifecycle and minimal component;
- P3: retained overlay bridge in software and OpenGL callbacks;
- P4: bounded job, cancellation and shutdown;
- P5: public navigation capability compile/runtime probe;
- P6: harmless helper process supervision;
- P7: child package install/update below host-private data.
