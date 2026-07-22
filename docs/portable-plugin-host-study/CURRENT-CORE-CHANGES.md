# Current modified-core implementation inventory

Baseline: OpenCPN `Release_5.14.0` at `91f3b674366068a6ecd61a5e9aba204bba85f57e`.
Reference implementation: `f85835669f97e3d913e4b26a531e47ae1f061b00`.

The comparison contains 88 changed paths, 27,733 insertions and five
deletions.  Most paths are new runtime, package, documentation or test files;
the permanent coupling to stock OpenCPN is concentrated in nine stock core
files plus one unrelated bundled-GRIB fix.  The machine-readable inventory is
in `current-core-changes.json`.

## Stock-file changes

| File / symbol | Added behaviour and internal dependency | Public plugin equivalent | Recommended destination |
|---|---|---|---|
| `CMakeLists.txt` | Builds and links the Rust bridge, portable hosts and packages into `opencpn`; adds experimental tests | A managed plugin has its own CMake/package target and private libraries | Move to host-plugin build. Keep portable library tests independent. |
| `gui/include/gui/pluginmanager.h` | Makes `PortablePluginManager` a child of the native plugin manager | Ordinary plugin object owns its runtime supervisor | Delete after extraction. |
| `gui/src/ocpn_app.cpp:BuildMainFrame` | Starts packages after native plugins load | `opencpn_plugin::Init()` and, if needed, `LateInit()` | Replace with native host-plugin lifecycle. |
| `gui/src/ocpn_frame.cpp:OnCloseWindow` | Stops jobs, helpers and components before native plugins deactivate | `DeInit()` is the supported plugin shutdown callback | Replace, retaining strict internal shutdown order. |
| `gui/src/ocpn_frame.cpp:OnToolLeftClick` | Routes ownerless toolbar IDs to portable actions | `OnToolbarToolCallback(id)` already routes every tool owned by the host plugin | Replace with host action registry. |
| `gui/src/ocpn_frame.cpp:RequestNewMasterToolbar` | Preserves expanded state and initial visible count during late rebuild | No direct plugin callback; dynamic public insertion/removal triggers the same toolbar machinery | Prototype on stock core. Retain only if the stock behaviour is proven defective; otherwise remove. |
| `gui/src/chcanv.cpp` | Copies cursor coordinates into the portable manager | `WANTS_CURSOR_LATLON` and `SetCursorLatLon()` | Replace directly. |
| `gui/src/pluginmanager.cpp` | Constructs/loads/stops runtime, dispatches actions and inserts retained drawing at the native overlay phase | Native plugin lifecycle, toolbar callbacks, `RenderOverlayMultiCanvas`, `RenderGLOverlayMultiCanvas`, `RequestRefresh` | Move to the host-plugin adapter. |
| `gui/include/gui/shapefile_basemap.h`, `gui/src/shapefile_basemap.cpp` | Adds cached point-in-polygon classification over private basemap files and types | Public `PlugIn_GSHHS_CrossesLand()` provides segment crossing, not a documented point classifier or hydrographic safety result | For display masking, redesign inside the plugin using a package-owned/coastline dataset. For authoritative chart safety, propose a generic public batch service if required. |
| `plugins/grib_pi/src/GribOverlayFactory.cpp` | Guards invalid projection coordinates, integer overflow, empty images and unsafe blur sizes | Not part of the portable runtime | Keep as an unrelated stock-plugin fix; exclude from feasibility claims. |

## Added implementation groups

| Component | Current dependencies | Host-plugin disposition |
|---|---|---|
| `PortablePluginManager` | Private plugin-manager `AddToolbarTool`, global position/config/platform objects, native public navigation helper functions, canvas top-frame refresh | Move with redesign. Use only exported plugin APIs and plugin callbacks; never create ownerless tools. |
| `PortableEnvironmentHost` | wxWidgets UI, helper processes, package resources, configuration, retained drawing, basemap classifier | Move most code into the host plugin. Replace direct globals and land classifier; split UI, helper supervisor and dataset service into modules. |
| `PortableWeatherRoutingHost` | wxWidgets UI, public-style route/waypoint value functions, route creation, chart crossing batches, environmental provider | Move most code. Marshal navigation through the adapter and retain immutable value snapshots. Chart semantics remain a separately classified limitation. |
| `portable_polar` | File/value parsing only | Move unchanged into a plugin-private library. |
| `portable_service_version` | Standard C++ only | Move unchanged. |
| `portable_ui_menu` | Validated JSON menu model and wx menu adapter | Move unchanged apart from ownership wiring. |
| Rust Wasmtime bridge | C callback table only; no OpenCPN headers or wx types | Move unchanged into a plugin-private static/shared library, subject to unload and symbol-visibility tests. |
| WIT and Rust components | Host-value interfaces only | Move unchanged initially. Evolve action declarations without exposing numeric toolbar IDs to guests. |
| Package build/install tools | Filesystem, ZIP, hashes and Ed25519 verification | Move unchanged initially; select plugin-private state roots and add production revocation later. |
| Environmental helpers | Pipe/job protocol and target-qualified resources | Repackage below the managed host plugin; containment and signing remain platform gates. |
| Tests and conformance | Several targets currently link the modified executable | Split into runtime-library tests, public-API host-plugin tests and behavioural parity tests against the reference executable. |

## Internal-state findings

The current host reaches internal OpenCPN state in four ways:

1. Direct globals for vessel position and configuration.
2. A private `PlugInManager` method used to create ownerless portable tools.
3. Direct top-frame refresh and lifecycle calls.
4. Private shapefile basemap data for point classification.

The first three have supported public plugin equivalents.  The fourth does
not have an equivalent with the same semantics; it should not be confused
with the public GSHHS segment-crossing helper, and neither is authoritative
chart/depth safety.

## Reusable versus accidental coupling

- The Wasmtime bridge, WIT, package tools, service-version parser, polar
  parser, components and most helper code are runtime-owned and independent.
- The environment and routing hosts are currently large wx/core-facing
  classes.  They are movable but should be decomposed rather than copied as a
  monolith.
- Ownerless toolbar tools, explicit frame click dispatch and direct startup
  calls are accidental consequences of embedding the supervisor in core.
- The retained-scene insertion point is a convenience; a native plugin already
  receives the relevant rendering callbacks.
- Land classification and future structured chart safety are the only current
  service family whose implementation fundamentally relies on chart/basemap
  knowledge not fully represented by the public API.

