# Portable Plugin Manager

Portable Plugin Manager is a conventional OpenCPN plugin which hosts
capability-limited, WebAssembly-based portable packages. OpenCPN itself remains
stock: the manager uses only the published `ocpn_plugin.h` boundary.

This implementation branch is intentionally stopped before cross-platform
release packaging or OpenCPN catalogue publication. See
`docs/portable-plugin-host-study/IMPLEMENTATION-WORKSPACE.md` for the frozen
baselines, validation classifications and workspace layout.

## Independent Linux build

```sh
cmake -S plugins/portable_plugin_manager_pi \
  -B /path/to/build \
  -DOPENCPN_SOURCE_DIR=/path/to/stock/OpenCPN \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /path/to/build
ctest --test-dir /path/to/build --output-on-failure
```

The plugin must also be loaded and exercised in a real stock OpenCPN process;
a successful module build alone is not runtime evidence.

## What the proof of concept includes

- signed archive inspection and transactional install/update/rollback;
- explicit, version-specific capability approval;
- enable, disable, unload and clean runtime shutdown;
- bounded Wasm execution, supervised helpers and failure diagnostics;
- one ordinary OpenCPN toolbar action per enabled portable package;
- declarative modeless and modal surfaces plus package-scoped file grants;
- modeless surfaces owned by the OpenCPN frame, with deferred foreground
  activation after toolbar events;
- versioned navigation, environmental, chart, routing and overlay services;
- iPolars, iGRIB and iWeatherRouting as complete reference workloads.

Disabling a package first cancels its work, removes its actions and surfaces,
then destroys its runtime instance. Enabling it again performs the installed
file audit and exact permission check before instantiation. Package failure is
contained and reported without taking down the Manager or stock OpenCPN.

This is deliberately an opt-in extension. A system which does not have enough
memory or CPU for its chosen portable workloads can run stock OpenCPN without
the Manager. See
`docs/portable-plugin-host-study/IMPLEMENTATION-VALIDATION.md` for measured
startup, idle and real weather-routing resource evidence.

## Renderer compatibility

The Manager declares both public overlay callbacks:

- `RenderOverlayMultiCanvas(wxDC&, ...)` is the renderer-neutral path used by
  OpenCPN's software chart frame. OpenCPN 5.15's Vulkan presenter uploads that
  completed frame after plugin overlays have been drawn, so portable overlays
  require no Vulkan handles, command buffers or private renderer classes.
- `RenderGLOverlayMultiCanvas(...)` is the legacy OpenGL path. Route geometry
  is emitted in OpenCPN's top-left overlay projection. The wxDC-based iGRIB
  workbench is converted through a bounded RGBA `glDrawPixels` compatibility
  surface, following the established desktop-plugin technique.

The OpenGL compatibility surface rejects dimensions above 8192 pixels or
16,777,216 total pixels and handles allocation failure without taking down
OpenCPN. Vulkan never uses this conversion and therefore avoids its transient
bitmap/upload cost.

Real-dataset tests have exercised both paths: Mesa OpenGL 4.6 on stock OpenCPN
5.14 and the Intel Iris Xe Vulkan presenter in the separate OpenCPN 5.15
working build. The Manager does not link to either renderer's private API.
