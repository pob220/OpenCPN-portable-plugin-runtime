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

## Per-plugin records

The following records use the pinned revisions in `SOURCE-LEDGER.md`. Line
numbers identify the audited snapshot, not a moving upstream branch.

### OCPN Draw

| Required field | Finding |
|---|---|
| Toolbar tools | Two: configuration and drawing/mode tools, registered by separate `InsertPlugInToolSVG` calls at `src/ocpn_draw_pi.cpp:553-563` (bitmap alternatives at 564-572) |
| Icon formats | SVG is preferred; bitmap fallback is retained. The drawing icon is replaced for boundary, EBL, DR, guard-zone and other modes using `SetToolbarToolBitmapsSVG`/bitmap equivalents at 580-682 and 4822 onward |
| Checked/enabled state | Independent checked state is changed repeatedly with `SetToolbarItemState`; no native enabled-state API is used |
| Cleanup | Explicitly removes both IDs in `DeInit` at 952-956—the strongest audited child-action cleanup precedent |
| Public APIs | Toolbar insertion/removal/state/artwork, context menus, DC/GL multi-canvas overlays, cursor/input, config, locale and plugin messages |
| Undocumented technique | Stores native integer IDs and uses an optional binary inter-plugin API after JSON discovery; neither is safe portable-child identity/ABI |
| Packaging/cross-platform | Template-derived managed packaging with Linux/Windows/macOS/Flatpak/Android-era scripts and bundled support libraries |
| UI/rendering | Large modeless/configuration UI, semantic chart-object editing, DC and GL multi-canvas rendering at 3433-3517 |
| Background work | Primarily UI/render/object management; not selected as the scheduler model |
| Messaging | Announces readiness and handles the established OCPN Draw JSON protocol; exposes a binary API to compatible native consumers |
| Known limitations | Global/native object ownership, transient tool IDs and unchecked binary caller contracts are not suitable guest boundaries |
| Host lesson | Multiple distinct actions and explicit per-ID cleanup are proven; replace raw IDs and binary pointers with logical keys and bounded values |

### Weather Routing

| Required field | Finding |
|---|---|
| Toolbar tools | One check/visibility tool via SVG with bitmap fallback at `src/weather_routing_pi.cpp:233-242` |
| Icon formats | SVG preferred, embedded bitmap fallback |
| Checked/enabled state | `SetToolbarItemState` mirrors window visibility at line 731; no independent native enabled state |
| Cleanup | `DeInit` stops timers/monitor, closes and destroys the modeless workbench, and processes pending events at 282-328; it relies on core plugin teardown for its tool container |
| Public APIs | Route retrieval/creation, cursor/position data, context items, config, DC/GL overlays and plugin messages |
| Undocumented technique | Exchanges convention-based synchronous JSON with GRIB, climatology and OCPN Draw; binary OCPN Draw use depends on peer-native ABI compatibility |
| Packaging/cross-platform | Managed template, target libraries and CI scripts for desktop, Flatpak and both Android ABIs; scripts prove build intent, not current runtime support |
| UI/rendering | Sophisticated modeless routing workbench and both `RenderOverlay`/`RenderGLOverlay` at 637-655 |
| Background work | Joinable route-map workers in `src/RouteMapOverlay.cpp`; current shutdown/event-yield patterns are feasibility evidence, not the proposed scheduler contract |
| Messaging | Sends GRIB timeline/climatology requests and OCPN Draw exclusion-boundary messages; receives provider responses in `SetPluginMessage` |
| Known limitations | Provider discovery/authentication and message ownership are conventional rather than typed; chart-safety semantics remain plugin-defined |
| Host lesson | iWeatherRouting's principal workload is publicly reachable; copy value semantics and capability breadth, not event-yield/global lifetime patterns |

### Radar

| Required field | Finding |
|---|---|
| Toolbar tools | One Radar tool at `src/radar_pi.cpp:337-340` |
| Icon formats | SVG normal/rollover/toggled; dynamic status artwork is replaced with `SetToolbarToolBitmapsSVG` at 2318-2348 |
| Checked/enabled state | Checked state follows radar visibility/status; no public per-tool enabled operation |
| Cleanup | `DeInit` stops timers and locator/receive workers, shuts down all radar instances, removes five context items and destroys dialogs/state at 447-524; tool container cleanup is left to core |
| Public APIs | High-rate position/AIS input, toolbar/context UI, multi-canvas GL/software overlays, config and plugin messaging |
| Undocumented technique | Native vendor/network libraries and global settings are tightly coupled to the plugin process; not a sandbox model |
| Packaging/cross-platform | Managed CMake, explicit Flatpak manifest/build, desktop/Android CI paths and bundled native radar libraries |
| UI/rendering | Multiple radar control windows and high-rate overlays; `RenderGLOverlayMultiCanvas` receives canvas index/priority at 1501 onward |
| Background work | Joinable receiver and discovery `wxThread` classes plus explicit shutdown/wait sequencing |
| Messaging | Receives plugin messages, principally for navigation/provider integration |
| Known limitations | High native dependency and network-thread risk; some work is triggered from render/timer cadence and must not be copied into a guest callback path |
| Host lesson | Publish prepared snapshots and bound render work; a native plugin can sustain demanding networking/rendering but must own strict shutdown |

### ShipDriver

| Required field | Finding |
|---|---|
| Toolbar tools | One check tool, SVG or bitmap selected at `src/shipdriver_pi.cpp:154-163` |
| Icon formats | SVG preferred with decoded bitmap fallback |
| Checked/enabled state | Checked state tracks modeless dialog visibility at 200, 344 and 439 |
| Cleanup | `DeInit` persists geometry, closes streams, stops/disconnects its timer, destroys UI and clears state at 172-226; relies on core tool-container cleanup |
| Public APIs | NMEA/AIS input, cursor, messages, preferences, config, software/GL overlay and refresh |
| Undocumented technique | No critical private OpenCPN dependency found; direct wx/native objects still make it ordinary native code rather than a child sandbox |
| Packaging/cross-platform | Compact managed-template repository with Flatpak manifest and Android/desktop build scripts |
| UI/rendering | Modeless simulator/driver dialog and simple overlays; explicit Android move/resize handling exists in its GUI implementation |
| Background work | GUI declares worker/timer activity; shutdown explicitly disconnects timer to avoid an exit crash |
| Messaging | Implements `SetPluginMessage` for native-plugin coordination |
| Known limitations | One-action/static lifecycle does not prove dynamic child registration; cleanup illustrates timer/event hazards |
| Host lesson | Use SVG-first/raster fallback, persist host-owned UI geometry and make timer disconnection part of the shutdown barrier |

### testplugin

| Required field | Finding |
|---|---|
| Toolbar tools | One check tool with SVG or bitmap at `src/testplugin_pi.cpp:250-256` |
| Icon formats | Separate SVG normal/rollover/toggled assets plus bitmap fallback |
| Checked/enabled state | Independent checked state is demonstrated at 467-472; no enabled state |
| Cleanup | `DeInit` closes the modeless control UI and saves config at 304-315; tool cleanup is left to core |
| Public APIs | Broad API example: lifecycle/LateInit, toolbar, preferences, messages, context/actions and navigation demonstrations |
| Undocumented technique | Its OCPN Draw binary API examples assume compatible native structures and trust; intentionally a test surface, not a security design |
| Packaging/cross-platform | Current template family with bundled API/support libraries, Flatpak and Android/desktop scripts |
| UI/rendering | Modeless API exercise/control UI; representative breadth rather than production render workload |
| Background work | Not selected as a substantial job scheduler example |
| Messaging | Announces readiness in `LateInit`, sends/receives OCPN Draw JSON and exercises binary discovery |
| Known limitations | Example code optimises API coverage, not bounded inputs, permission policy or failure isolation |
| Host lesson | Valuable compile/API oracle and packaging baseline; retain only supported value-based patterns |

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
