# Representative plugin audit

Status: source audit complete on 2026-07-22. Revisions are pinned in the
[source ledger](SOURCE-LEDGER.md). Line references below are paths within each
pinned repository.

## Selection and evidence

| Plugin | Why selected | Directly observed pattern | Applicable lesson |
|---|---|---|---|
| OCPN Draw | Strongest multi-action and rendering example | `src/ocpn_draw_pi.cpp` registers `m_config_button_id` and `m_draw_button_id` with two `InsertPlugInToolSVG` calls, changes the second tool's SVG for several modes, maintains independent checked state, and removes both in `DeInit` | A single ordinary plugin can own multiple distinct toolbar tools. The host should mirror its explicit per-ID cleanup, but use a logical-key registry rather than globals |
| Weather Routing | Closest navigation workload | `src/weather_routing_pi.cpp` uses one check tool, modeless UI, context actions, DC/GL overlays and messaging; `src/WeatherRouting.cpp` reads and creates routes through public APIs; `src/RouteMapOverlay.cpp` uses joinable workers | Most iWeatherRouting facilities have production public-API precedents. Existing polling/sleep shutdown patterns should be replaced with bounded cancellation |
| Radar | High-rate rendering and background I/O | `src/radar_pi.cpp` implements multi-canvas GL and software callbacks, changes toolbar SVGs and checked state; receive and discovery classes derive from joinable `wxThread` | Expensive work belongs off the UI/render callbacks. State snapshots must be published to rendering code with clear ownership |
| ShipDriver | Small cross-platform managed plugin | `src/shipdriver_pi.cpp` selects SVG or PNG, registers a check tool, persists dialog state, updates the tool state and uses plugin messages; its GUI declares a worker thread | SVG-first plus raster fallback is established practice. Plugin shutdown must stop timers/threads before destroying UI |
| testplugin | Template and interoperability surface | `src/testplugin_pi.cpp` registers a toolbar tool, owns modeless UI, sends addressed JSON conventions through broadcast plugin messaging and exposes API examples | Useful as a compile/API oracle, not as a security or concurrency design to copy wholesale |

## Multiple toolbar tools

OCPN Draw is the decisive production example. It calls the public insertion
API twice from one plugin instance. Both calls pass the same `this` owner but
receive different numeric IDs and distinct labels, tooltips and SVG paths.
OpenCPN dispatches both IDs to the same `OnToolbarToolCallback(int)` owner; the
plugin is responsible for resolving the ID. OCPN Draw also shows that
`GetToolbarToolCount()` is not an enforced allocation contract: it returns one
while the plugin registers two tools.

This proves multiplicity and ownership, but not stable identity. Every audited
plugin stores process-local integer IDs. None treats those IDs as persistent
package identity, and none demonstrates that OpenCPN remembers a plugin tool's
placement across rebuilds.

## Toolbar state and cleanup

- OCPN Draw changes normal/rollover/toggled SVG assets at runtime with
  `SetToolbarToolBitmapsSVG` and changes checked state with
  `SetToolbarItemState`.
- Radar similarly replaces SVG artwork and sets toggle state.
- ShipDriver and Weather Routing maintain a single independent check state.
- OCPN Draw explicitly calls `RemovePlugInTool` for both tools. Other plugins
  often rely on OpenCPN unloading their containers; the explicit approach is
  the safe model for dynamic child actions.
- None of the audited plugins uses a public per-toolbar-tool enable/disable
  call because API 1.21 has no such call.

## UI, rendering, navigation and messaging

Weather Routing is direct evidence that an ordinary plugin can open complex
modeless UI, subscribe to cursor/navigation events, render software and GL
overlays, read a route by GUID and create routes. It also consumes GRIB and
climatology data through JSON plugin messages. OCPN Draw demonstrates more
complex context UI, multiple overlays and a sizeable message protocol. Radar
demonstrates that one plugin can maintain network worker threads and render on
multiple chart canvases.

These patterns establish API reach, not a licence to copy their internals.
Some use blocking sleep loops during shutdown and global mutable state. The
runtime host needs explicit cancellation, immutable render snapshots and a
single UI-thread adapter.

## Packaging and targets

The audited repositories use the OpenCPN managed-plugin build conventions:
platform-specific tarballs plus metadata consumed by the catalogue. Current
templates contain Linux/Flatpak, Windows, macOS and ARM target logic; Weather
Routing also carries Android toolchain paths. This shows that a native host can
use the managed distribution channel. It does not prove that bundled Wasmtime,
native helpers, child-package downloads or Android execute correctly on every
target; those remain host-specific validation obligations.

## Unsafe or undocumented patterns not adopted

- Raw plugin numeric tool IDs are not persistent child identity.
- Broadcast JSON strings do not provide authentication, confidentiality,
  ownership or binary lifetime guarantees.
- Joinable threads and sleep-loop polling are examples of feasibility, not the
  recommended shutdown protocol.
- Plugin-private copies of OpenCPN internals or private headers would turn API
  reach into an ABI dependency and are excluded from the production design.
