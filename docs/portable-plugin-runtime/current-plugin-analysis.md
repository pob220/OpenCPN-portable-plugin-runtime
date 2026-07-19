# Current plugin and workload analysis

## Audit scope and isolation

This analysis is anchored to stock OpenCPN 5.14.0 at commit `91f3b674366068a6ecd61a5e9aba204bba85f57e`, not the modified 5.15.0 checkout on this machine. The test launch uses `/home/paul/Test-OpenCPN/bin/launch-test-opencpn`, a staged binary under `/home/paul/Test-OpenCPN/app`, dedicated `HOME`/XDG directories, `--portable`, a dedicated config directory and only `/home/paul/Test-OpenCPN/config/plugins/lib`. The normal 5.15.0 launcher uses `/home/paul/src/OpenCPN/build/opencpn` and normal user plugin paths. Neither application was launched during research.

The 5.14.0 worktree already contained an unrelated modification to `plugins/grib_pi/src/GribOverlayFactory.cpp`; it was neither used as evidence nor changed.

Primary source baselines inspected on 2026-07-19:

- [OpenCPN 5.14.0 source](https://github.com/OpenCPN/OpenCPN/tree/91f3b674366068a6ecd61a5e9aba204bba85f57e)
- [pob220/xgrib_pi at `f557894`](https://github.com/pob220/xgrib_pi/tree/f5578941019bc5f3196df5dfb61138f91ea64cd9)
- [pob220/weather_routing_pi `routing-engine-refactor` at `152de494`](https://github.com/pob220/weather_routing_pi/tree/152de49438a14704f54dfad8f1f1701126417aa3)
- [pob220/OpenCPN-weather-routing `safer-renderer`](https://github.com/pob220/OpenCPN-weather-routing/tree/safer-renderer)
- [JavaScript_pi at `e62883`](https://github.com/antipole2/JavaScript_pi/tree/e62883)
- current [testplugin](https://github.com/jongough/testplugin_pi) and [shipdriver](https://github.com/Rasbats/shipdriver_pi) template families
- official [5.14.0 release](https://opencpn.org/OpenCPN/about/ver514.0.html), [developer manual](https://opencpn-manuals.github.io/main/ocpn-dev-manual/0.1/pm-plugin-api-versions.html) and catalogue/deployment guidance.

Repository searches did not identify an accepted current WebAssembly/portable-plugin RFC. That absence should be confirmed by maintainers during review; it is not evidence that the topic has never been discussed.

## Native loading and ABI

### `include/ocpn_plugin.h`

API 1.21 is a large inheritance-based C++ contract. It contains `wxString`, `wxBitmap`, `wxMenuItem`, `wxFileConfig`, `wxDC`, `wxGLContext`, native plugin chart classes, C++ list/array types and pointer-based ownership. Navigation APIs construct plugin waypoint/route/track structures and return collections/pointers. Chart APIs expose XML/wx containers and chart-directory/database mutation. Overlay callbacks receive native DC or GL state. These concepts cannot form a stable portable boundary.

### `model/include/model/plugin_loader.h` and `model/src/plugin_loader.cpp`

`PluginLoader` discovers platform library paths through `PluginPaths`, applies load stamps, opens a `wxDynamicLibrary`, resolves `create_pi` and `destroy_pi`, and stores the result in `PlugInContainer`. Compatibility is negotiated with integer API versions and `dynamic_cast` through the `opencpn_plugin_105` … `_121` inheritance chain. Enable/deactivate/unload all assume an in-process native object and shared C++ runtime.

Portable design consequence: do not add Wasm cases to `PlugInContainer` or synthesize an `opencpn_plugin`. Add a parallel package registry/supervisor and combine only their presentation models.

### Installation and catalogue

- `model/include/model/plugin_handler.h` and `model/src/plugin_handler.cpp` install target-specific tarballs with libarchive into platform-dependent locations, track installed metadata and select by OS/target/version.
- `model/include/model/catalog_parser.h` maps XML to `PluginMetadata`: name, version/release, API, target, target version/architecture, tarball URL and checksum. It has no package signature, capability, portable API range or revocation semantics.
- `model/include/model/catalog_handler.h` and `model/src/catalog_handler.cpp` download, merge and cache catalogue XML.
- `gui/src/catalog_mgr.cpp` manages catalogue selection/update.

The current mechanism should remain for native plugins. Portable records need distinct schema and installer paths; a catalogue may display both kinds but must not interpret a native checksum as a trust signature.

### Plugin Manager and lifecycle integration

`gui/include/gui/pluginmanager.h` and `gui/src/pluginmanager.cpp` implement `PlugInManager`, including activation, settings panels, toolbar/menu insertion, message dispatch, overlay rendering and plugin downloads. `gui/src/options.cpp` embeds `PluginListPanel` and `CatalogMgrPanel`. Startup constructs the loader in `gui/src/ocpn_app.cpp`; frame pre-shutdown/deactivate/unload paths are in `gui/src/ocpn_frame.cpp`.

`PlugInManager` calls native `RenderOverlay` and `RenderGLOverlay` synchronously with `wxDC`/`wxGLContext`. Canvas call sites are in `gui/src/chcanv.cpp` and `gui/src/gl_chart_canvas.cpp`. Portable overlays instead need a scene store called at the same canvas phases.

## Communications and remote control

`model/include/model/plugin_comm.h` and `model/src/plugin_comm.cpp` broadcast `message_id` plus string body to every interested native plugin and bridge `PluginMsg` to `NavMsgBus`. It is useful as a compatibility bridge but has no typed schema negotiation, authenticated provider identity, backpressure or per-consumer permission. The new service registry must not be based on these strings.

`model/include/model/rest_server.h`, `model/src/rest_server.cpp` and `gui/src/rest_server_gui.cpp` demonstrate a core service separated from a GUI adapter and dependency-injected route callbacks. However, internal `RouteCtx` callbacks return raw `Route*`, `Track*` and `RoutePoint*`. Its remote authentication/wire model and object access are not a portable plugin ABI. `test/rest-tests.cpp` is useful precedent for headless adapter tests.

The newer `NavMsgBus` (`model/include/model/comm_navmsg_bus.h` and related `comm_navmsg` files) supplies typed internal messages, but public portable navigation values still need independent schemas and stable source identifiers.

## Core data and renderer ownership

| Domain | Current implementation areas | Portable implication |
|---|---|---|
| routes/waypoints/tracks | `model/include/model/{route,route_point,track,routeman,navobj_db}.h`, corresponding sources, `gui/src/navutil.cpp` | expose UUID/revision value records and transactions; core maps them to internal objects |
| chart database | `gui/include/gui/chartdb.h`, `gui/src/chartdb.cpp`, `gui/include/gui/chartbase.h`, S57/CM93 implementations | core retains chart/cache ownership and thread rules; publish coverage/safety evidence only |
| rendering | `gui/src/{chcanv,gl_chart_canvas,pluginmanager}.cpp` | retained scene is projected/rendered by host; no graphics contexts |
| configuration | `GetOCPNConfigObject()` implemented in `gui/src/ocpn_plugin_gui.cpp` returns `wxFileConfig*` | namespaced typed settings service replaces shared mutable config |
| downloads | plugin manager and model downloader/peer code use wxCurl/libcurl | make a new policy-enforcing HTTP service; do not copy legacy TLS behavior |

The `safer-renderer` chart-safety work validates important semantics—land/drying/too-shallow/no-data/error outcomes, vector/CM93/GSHHS sources, route masks and main-thread prewarming followed by worker cache reads. It also concentrates thousands of lines in `gui/src/ocpn_plugin_gui.cpp` and exposes native structs in `ocpn_plugin.h`. Treat it as algorithm/performance evidence, then extract a dedicated service rather than freezing that prototype surface as the portable API.

## xGRIB workload

xGRIB retains the familiar native wxWidgets viewer/timeline and DC/GL overlays, while its generator launches an `environmental-grib` process. Jobs use schema-versioned JSON, JSONL progress, password environment transfer with redaction and atomic results. Linux packaging includes a private helper runtime, ecCodes definitions/samples and PROJ data; Windows/macOS still require native dependency and signing work.

Observed dependency/work split:

- viewer: native OpenCPN/wx UI, GRIB timeline and overlays, `GRIB_*` string messages;
- generator: ecCodes, NetCDF, PROJ, curl, Qhull, JSON, bzip2, Blosc, libzip, libsodium, zstd and source-specific logic;
- operations: download, archive/decompress, NetCDF ingest, regrid/merge, TPXO/XTD data, validation and GRIB write.

This is strong evidence for the mixed placement in the RFC. Provider orchestration, metadata and portable processing can be a component; sampling should be a batched service; the first production-grade codec/generator should be a supervised helper. Forcing the entire native stack into Wasm would transfer build and data-file maintenance into the plugin SDK without proving operational value.

## Weather Routing workload

The `routing-engine-refactor` branch introduces scenario/result/diagnostic structures and headless JSON scenarios while retaining some wx types. It covers multi-leg and multi-waypoint routing, departure candidates, currents, chart safety, reverse/final-approach recovery, alternatives and stability corridors. Its documented rule that worker chart API calls are zero after main-thread cache preparation directly informs the chart executor/cache design.

The workload makes fine-grained boundary calls unacceptable. A route search may expand many states, each requiring multiple weather fields and segment checks. The interface must submit vectors of points/times/fields and vectors of segments, then return packed/structured batches. Progress and partial alternatives are streams; cancellation must interrupt both guest compute and host batches. Headless scenario files should become conformance fixtures after removing internal/wx representations.

## JavaScript and scripting evidence

JavaScript_pi embeds Duktape in a native plugin and wraps navigation, routes, NMEA, files, URL access and optional sockets. It has execution timeout/fatal handling and makes scripting approachable, but the host plugin remains ABI-specific, in-process and broadly privileged; script failure containment depends on the native embedder. It proves demand for higher-level authoring, not a secure portable tier.

Lua has a small embeddable implementation and JavaScript has strong developer reach, but either still needs the same stable host services, quotas and package trust. Python provides reach but bundles a large interpreter and many useful packages depend on native extensions. Current `componentize-py` explicitly bundles CPython and documents limitations ([componentize-py](https://github.com/bytecodealliance/componentize-py)). High-level languages should target the component tier when their toolchains pass conformance; they should not define a second public API.

## Templates and release engineering

The current testplugin/shipdriver CI matrices build combinations including MSVC, macOS universal binaries, several Debian distributions and architectures, Flatpak x86-64/AArch64 runtimes and sometimes ARMHF/Android. This is excellent infrastructure for native fallback and future runtime tests, but illustrates that central CI does not make one artifact: it manufactures and signs a matrix of native archives.

A centrally maintained build service remains valuable for native-only plugins and helper payloads. It is complementary, not the portable architecture.

## Classification of existing concepts

### Safe to adapt through new value services

- vessel/navigation snapshots and decoded navigation-message subscriptions;
- route/waypoint/track list/get/create/update using UUIDs and revisions;
- toolbar/menu actions as declarative registrations;
- settings/localisation/notifications;
- user-selected file import/export;
- selected message ecosystems through explicit typed adapters;
- route overlay concepts expressed as retained primitives.

### Replace, do not wrap directly

- `wxFileConfig*` with typed settings/credential services;
- `PlugIn_Route`/pointer collections with immutable records and transactions;
- chart XML/database objects with queries and structured evidence;
- broadcast strings with typed discoverable services;
- synchronous callbacks with async requests/events;
- `wxDC`/OpenGL callbacks with retained scenes;
- direct curl/socket/file access with capability brokers.

### Remain native-only initially

- custom native chart classes and proprietary chart SDKs;
- direct OpenGL/Metal/Direct3D/Vulkan rendering;
- native window/control injection beyond host declarative UI;
- arbitrary device drivers, raw OS handles and unrestricted sockets;
- plugins requiring foreign native ABI callbacks or in-process hardware SDKs.

## Conclusions

The hardest work is not embedding a runtime. It is extracting service ownership, threading and semantics from API calls that currently rely on shared C++/wx state. That work is still valuable: the same services make core code more testable and can give native plugins safer wrappers. The prototype must therefore validate one narrow service path before broad interface design.
