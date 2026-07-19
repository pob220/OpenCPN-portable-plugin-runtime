# OpenCPN 5.14.0 core modification map

These are proposals, not implementation instructions. New service classes deliberately live outside `ocpn_plugin.h` and have no Wasmtime dependency. `OCPN_ENABLE_PORTABLE_PLUGINS` is a CMake option defaulting `OFF`; `EnablePortablePluginsExperimental` is a runtime preference defaulting false. Both must be true to discover packages.

## Build, startup and lifecycle

| Existing file/class | Proposed change and public surface | Disabled behavior | Tests / rollback | Burden |
|---|---|---|---|---|
| root `CMakeLists.txt` (options near existing build switches; GUI plugin sources and tests later in file) | Add default-OFF option and conditionally include `portable_runtime` target/Wasmtime shim. Produce a dependency/SBOM entry. | No runtime target linked, no binary-size or startup change. | configure/build both ways on all CI; remove conditional target to roll back. | M: Rust/CMake and platform linkage ownership. |
| `model/CMakeLists.txt` | Under flag, add GUI-neutral package, manifest, service and supervisor sources. Core service implementations themselves may later be unconditional once useful internally. | Existing model source list unchanged. | model unit tests with fake adapters; source-list revert. | L–M. |
| `gui/src/ocpn_app.cpp` (`PluginLoader` construction around current startup) | Construct `PortablePluginRegistry`/`PortablePluginSupervisor` only after runtime preference is read; discovery must not instantiate. | Exact current native startup path. | startup ordering and disabled trace tests; delete guarded block. | L. |
| `gui/src/ocpn_frame.cpp` (pre-shutdown, deactivate and unload paths) | Add explicit portable suspend/disable/stop before UI/chart destruction, with deadline and forced instance termination. | Exact native shutdown order. | trap/hung job/helper/shutdown tests; remove guarded calls. | M: lifecycle correctness. |
| proposed `model/include/model/portable/{package_manifest,package_store,portable_registry,portable_supervisor}.h` and sources | Bounded manifest inspection, signed atomic install slots, state machine, compatibility and permissions. Public internal interfaces contain ordinary value DTOs. | Not compiled or constructed. | malformed corpus, state-machine/property tests, atomic rollback. | H: permanent security surface. |
| proposed `model/src/portable/wasmtime_adapter.rs` plus narrow C ABI header, or equivalent C++ adapter if proven | Runtime-specific engine/store/linker, WIT bindings, fuel/epoch/memory limits and trap translation. No runtime type escapes adapter. | Absent from build. | runtime conformance, fuzz, sanitizers, patch-upgrade test. | H: runtime update expertise. |

Do **not** modify `model/include/model/plugin_loader.h`, `model/src/plugin_loader.cpp`, `PlugInContainer` or native `create_pi`/`destroy_pi` negotiation for the prototype. Shared lifecycle presentation belongs above both registries.

## Plugin Manager, catalogue and install

| Existing file/class | Proposed change and public surface | Disabled behavior | Tests / rollback | Burden |
|---|---|---|---|---|
| `gui/include/gui/pluginmanager.h`, `gui/src/pluginmanager.cpp` (`PlugInManager`, list panels) | Add a `PluginPresentation` model/facade for native or portable entries; delegate portable enable/disable/permission UI to registry. Do not put portable objects in the native plugin array. | Current native list/actions and overlay dispatch. Portable branch not constructed. | GUI model/action tests with fake registries; remove facade adapter while native model remains. | M–H: central UI code. |
| `gui/src/options.cpp` (`PluginListPanel`, `CatalogMgrPanel` integration) | Show portable badge, API compatibility, signature/trust and requested/granted permissions; experimental warning. | No new rows/options. | screenshots/event tests, keyboard/accessibility; guarded panel fields removable. | M. |
| `gui/src/catalog_mgr.cpp` | Show portable catalogue trust/update status and revocation freshness separately from native XML catalogue. | Existing catalogue dialog. | offline/expired/bad-signature fixtures. | M. |
| `model/include/model/catalog_parser.h`, `model/src/catalog_parser.cpp` | Prefer a new `PortableCatalogEntry` parser rather than extending target-specific `PluginMetadata`; only common display DTO is shared. | Native XML parser untouched. | schema/unknown-field/size tests. | M. |
| `model/include/model/catalog_handler.h`, `model/src/catalog_handler.cpp` | Reuse downloader scheduling where safe, but add separate signed metadata state and rollback protection behind a `PortableCatalogClient`. | Native merge/cache path unchanged. | TUF-style expiry/rollback/mirror attacks; delete separate client. | H: update security. |
| `model/include/model/plugin_handler.h`, `model/src/plugin_handler.cpp` | Keep native tar installer. Portable `PackageStore` performs path-safe deterministic ZIP extraction, signature verification, atomic version slots and rollback. | Native install locations/metadata unchanged. | traversal, symlink, zip-bomb, interrupted install tests. | H. |

## Service layer

| Existing owner/file | Proposed service/classes | Reason and public value interface | Thread/lifecycle | Tests and rollback |
|---|---|---|---|---|
| navigation model: `model/include/model/{route,route_point,track,routeman,navobj_db}.h`, sources; `gui/src/navutil.cpp` | `model/include/model/services/navigation_service.h`; `NavigationServiceImpl`; DTOs `NavSnapshot`, `PublicWaypoint`, `PublicRoute`, `PublicTrack`, `ObjectRevision` | Stop exposing internal pointers. UUID/revision CRUD, apply, import/export and delta subscriptions. | snapshot executor; mutations serialised to model/main owner; service app-lifetime. | fake nav repository, stale revisions, transactions/GPX. Initially constructed only for portable flag; service files can be removed without model schema changes. |
| position/nav messaging: `model/include/model/comm_navmsg*.h`, decoder/observable sources | adapter into `navigation@1` snapshots/subscriptions | stable selected-source/quality records rather than NMEA/wx objects. | bus callbacks copy bounded values, never enter guest. | source selection/drop/rate tests. |
| charts: `gui/include/gui/chartdb.h`, `gui/src/chartdb.cpp`, `gui/include/gui/chartbase.h`, S57/CM93/GSHHS implementations | `model/include/model/services/chart_safety_service.h` plus GUI/chart adapter; `CoverageResult`, `SegmentBatch`, `SafetyEvidence`, `PreparedRouteCache` | core retains chart objects; publish structured clear/unsafe/unknown/missing/detail/conflict evidence. Extract algorithms from experimental `safer-renderer`, not its native structs. | preparation/chart access on required main/chart executor; immutable cache worker-readable; revision invalidation. | synthetic/vector/CM93/GSHHS fixtures, thread assertions, cache invalidation, conservative no-data. Keep old experiment/native API untouched until parity; remove new adapter to roll back. |
| environment currently split among GRIB/native plugins | `model/include/model/services/environment_service.h`; dataset/provider registry and sampling DTOs | common metadata/units/provenance/quality, immutable dataset resources, batch sample/chunk/stream. | providers own dataset backing; registry owns identity; bounded worker calls. | analytic datasets, provider loss, malformed metadata, batch benchmarks. Entire registry removable behind flag initially. |
| jobs | `model/include/model/services/job_service.h`; bounded scheduler/cancel/deadline/event types | one cancellation/progress/failure model for portable and future core jobs. | app-owned pools; per-plugin leases; deterministic shutdown. | fairness, races, deadline, leak and shutdown. |
| settings/config: current `GetOCPNConfigObject()` implementation in `gui/src/ocpn_plugin_gui.cpp` | `settings_service.h` and namespaced transactional backend | prevent access to global `wxFileConfig`; typed quotas and migrations. | serial storage executor; app lifetime. | corruption/atomicity/migration/quota; native API remains. |
| file dialogs/storage paths scattered in GUI/model | `storage_service.h`; GUI file-picker adapter; private/cache/temp/user-selected file capabilities | no ambient paths; map portals/bookmarks/tokens per OS. | async; grant/resource lifetimes explicit. | fake picker, traversal/race, Flatpak portal adapter. |
| download/curl paths in plugin manager/model | `http_service.h`; host client adapter | policy-controlled HTTP streams, TLS/proxy/redirect/rate/cancel and credential injection. | network executor; request resources. | local TLS/proxy server, redirect escape, cancellation and limits. Do not alter legacy downloader initially. |
| OS config/secret facilities | `credential_service.h` with Windows Credential Manager/DPAPI, macOS Keychain and Linux Secret Service/portal adapters where available | secrets separated from settings and scoped to provider/plugin identity. | host broker; locked/unavailable is typed error. | fake vault conformance/redaction; backend may be absent without fallback to plaintext. |
| `model/include/model/plugin_comm.h`, `model/src/plugin_comm.cpp` | new `typed_service_registry.h`; explicit native-message bridge adapter only for selected schemas | authenticated provider/version discovery and typed calls; do not expand string broadcast. | registration tied to enabled lifecycle; calls scheduled/bounded. | spoof/version/provider-removal/cycle tests. Existing messaging unchanged. |

Core service interfaces must not include WIT, Wasmtime, wxWidgets or plugin-loader types. WIT and future native C++ bindings adapt to them.

## UI and rendering

| Existing file/class | Proposed change | Public interface | Disabled behavior | Tests / rollback |
|---|---|---|---|---|
| toolbar/menu functions in `gui/src/pluginmanager.cpp` and native API | `PortableUiRegistry` plus wx adapter | versioned declarative actions/forms/panels and typed events; transactionally register/remove | existing native toolbar/menu functions only | action lifecycle, focus, localisation, accessibility, DPI; remove guarded registry |
| `gui/src/chcanv.cpp` DC render phases | call `PortableOverlaySceneStore::RenderDc()` at one documented layer when enabled | renderer consumes validated host scene model, not guest data directly | identical current render ordering | golden projection/clipping/z/alpha; compare disabled frame trace |
| `gui/src/gl_chart_canvas.cpp` GL render phases | equivalent `RenderGl()` using existing renderer abstractions | same scene semantics across backends | identical current GL path | backend parity, context loss; guarded call removable |
| native overlay dispatch in `gui/src/pluginmanager.cpp` | leave `RenderAllCanvasOverlayPlugIns`/GL dispatch intact; portable store is a sibling | no portable graphics context | unchanged | regression for native overlay ordering |
| proposed `gui/include/gui/portable/{declarative_ui,overlay_scene_store,overlay_renderer}.h` and sources | schema validation, retained scenes, incremental transactions, hit testing and event routing | `UiTree`, `Scene`, `ScenePatch`, stable node/hit ids | not compiled/constructed | fuzz complex geometry/text, node/byte/update quotas, destroy on trap/disable |

The first vertical slice needs only one toolbar/menu action and one geographic line/polyline scene. Panels, icons, text, hit testing and incremental updates should not be implemented until this path is stable.

## Package paths and platform adapters

Use a dedicated portable package root under OpenCPN's existing per-user data/config conventions, never native plugin library directories. Flatpak storage remains inside the application sandbox; user files are portal grants. Windows/macOS/Linux path selection belongs to `PackageStore`, not guest code. Optional helpers are selected by exact normalized target tuple and executed only from a verified immutable install slot.

Flatpak applications cannot access host processes/files/network by default, while the file chooser portal can grant selected files ([sandbox permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html), [FileChooser portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.FileChooser.html)). Helper support therefore depends on packaging the helper inside the Flatpak and on its allowed process/network model; escaping to a host-installed helper is out of scope.

## Test integration

- `test/CMakeLists.txt`: conditionally add manifest/package/service/supervisor tests; core service tests should run without Wasmtime where possible.
- `test/rest-tests.cpp`: reuse its dependency-injection style, not its object model.
- proposed `test/portable/fixtures`: signed-good packages, incompatible ranges, revoked identities, malformed ZIP/component/WIT returns, trap/spin/memory-grow components.
- proposed headless conformance executable links service fakes and runtime adapter, allowing plugin developers to run the same WIT vectors.
- GUI integration tests assert main-thread ownership and that trap/disable retracts all actions/scenes.
- each CI job builds flag OFF; selected platform jobs also build/run flag ON.

## Feature flag, rollback and change discipline

1. Flag OFF is the primary compatibility contract and must be tested on every PR.
2. Runtime preference false avoids package scanning and engine construction even in an enabled build.
3. New database/catalogue/package state uses separate files and directories; native metadata is never migrated.
4. Every stage is removable by deleting guarded call sites and the independent target. Do not alter nav/chart persistent schemas for the prototype.
5. If a runtime vulnerability cannot be patched promptly, a signed revocation/runtime-policy update can disable the portable tier while native plugins remain available.
6. No existing native plugin requires modification or recompilation.

## Maintainer hot spots

Highest continuing burden: runtime adapter/security upgrades, package/update trust, chart-safety semantics and Plugin Manager UX. Medium: cross-platform storage/credentials, retained renderer and service version adapters. Lower after stabilisation: basic navigation/settings/jobs interfaces. Ownership must be assigned before code merges; see [maintainer-impact.md](maintainer-impact.md).
