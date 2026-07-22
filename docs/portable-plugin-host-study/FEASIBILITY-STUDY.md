# Portable runtime host-plugin feasibility study

## Recommendation

Choose **Outcome B**: deliver the useful runtime as one ordinary managed
OpenCPN host plugin, prove it first on completely stock OpenCPN, and request a
small generic stable dynamic toolbar-action API for a broadly usable beta.
Keep Wasmtime in-process only for project-signed technology-preview packages;
preserve a process-neutral adapter and move execution/high-risk parsing into a
supervised broker before opening a third-party production ecosystem.

No current useful preview service needs a thin core broker. Authoritative
chart/depth/hazard queries and first-class child entries are separate future
core questions, not prerequisites for iGRIB/iWeatherRouting toolbar actions,
UI, overlays, jobs, storage, network brokering or navigation values.

Confidence is **high for the Linux stock-host architecture and toolbar
acceptance criterion**, medium for a desktop managed-plugin beta until target
builds execute, and low/unsupported for Android.

## Methodology and evidence boundary

The study:

1. diffed the modified proof against stock `Release_5.14.0`;
2. traced every stock-file change and the added runtime/package/helper groups;
3. inspected API declarations and implementation at both stock and current
   upstream revisions;
4. audited five current representative plugins and the managed catalogue;
5. built an isolated conventional public-API plugin and loaded it into an
   independently built unmodified stock executable;
6. linked the clean existing Wasmtime bridge, ran both components through the
   bridge smoke test, and ran iGRIB through the stock host;
7. exercised dynamic toolbar lifecycle, software/GL rendering, worker shutdown,
   public navigation/path reach, Linux helper tree cleanup and package policy;
8. classified 61 services and compared seven architectures with fixed weights.

External sources were accessed on 2026-07-22 and checked against source where
possible. Exact SHAs, API version, toolchain and official URLs are in
`SOURCE-LEDGER.md`. Facts marked as target plans are architectural inference,
not executed cross-platform evidence.

Experiments used `/tmp` sources, builds, portable profile/config and plugin
path. The user's OpenCPN 5.15 installation/profile was not touched. Clean
`git archive` inputs excluded concurrent iWeatherRouting changes.

## Source versions

- Study/reference base: `f85835669f97e3d913e4b26a531e47ae1f061b00`.
- Stock 5.14: `91f3b674366068a6ecd61a5e9aba204bba85f57e`.
- Upstream OpenCPN master inspected: `bc0e1ededbb10cd44feffc26b7989b6b990fa6eb`.
- Public plugin API: 1.21 at all three relevant source points.
- Existing bridge: Wasmtime 46.0.1; official documentation reported 48.0.0
  when accessed.
- Representative repository SHAs are pinned in the ledger.

## Existing modified-core architecture

The reference adds 88 paths, 27,733 lines and changes only nine meaningful
stock core files plus one unrelated bundled-GRIB defensive fix. Permanent
coupling is much smaller than total code volume:

- application/plugin-manager startup and shutdown own the supervisor;
- frame click dispatch routes ownerless numeric tools;
- plugin-manager rendering inserts retained scenes;
- canvas forwards cursor coordinates;
- host classes use direct globals/config/frame refresh;
- a new private basemap point classifier supports display filtering.

The Wasmtime C bridge, WIT, components, package tools, signatures, SemVer,
polar parser and helpers have no essential OpenCPN-core dependency. The two
large wx host classes are movable but should be decomposed. The exact
machine-readable inventory is `current-core-changes.json`; destinations and
internal-state analysis are in `CURRENT-CORE-CHANGES.md`.

### What moves unchanged

- WIT worlds and current component value interfaces (initially);
- Rust bridge/C ABI and engine limits (with packaging/failure caveats);
- iGRIB and iWeatherRouting component code;
- deterministic archive, checksum/signature and atomic installer logic;
- service version and polar parsing;
- helper executables/protocols as target payloads;
- most pure tests and conformance fixtures.

### What moves with redesign

- `PortablePluginManager` becomes a decomposed host supervisor and adapter;
- environment/routing hosts lose core globals/private include reach;
- ownerless numeric actions become host-owned public tools with a logical map;
- all guest execution leaves the UI/render callback path;
- UI/overlay state becomes validated immutable values;
- permission consent, revocation, credentials and package update UX mature;
- helper discovery/install paths move under the managed host package/private
  child root split.

### What is reduced or deferred

- point/segment coastline results remain advisory, not chart safety;
- native disabled toolbar visuals and child placement customisation remain
  reduced on stock API;
- child packages remain runtime-managed child extensions;
- Android and uniform native-helper containment are deferred.

## OpenCPN native plugin architecture

### Lifecycle and ABI

`model/src/plugin_loader.cpp` loads the native module, negotiates class/API
versions, calls `Init`, processes `LateInit`, stores capability flags, and
calls `DeInit` through deactivation/unload. The host can therefore use normal
startup, enable/disable, late activation and shutdown. There is no separate
asynchronous shutdown phase: its own jobs/helpers must reach a barrier before
`DeInit` returns.

OpenCPN stores only the outer native plugin's identity and enabled state. A
host can persist child state in the global `wxFileConfig` under its own prefix
and files below the public private-data root. Official installer documentation
states `GetPluginDataDir` is installer-owned/read-only; child downloads must
not alter it.

### Toolbar call path

`include/ocpn_plugin.h` exports insertion, removal, visibility, check-state and
icon replacement. `gui/src/ocpn_plugin_gui.cpp` forwards those to
`PlugInManager`; `gui/src/pluginmanager.cpp` creates one container per call and
assigns `m_plugin_tool_id_next++`; `ocpn_frame.cpp`/`toolbar.cpp` reconstruct
plugin-owned items; click handling resolves the container and invokes the same
owner's `OnToolbarToolCallback(id)`.

There is no per-plugin multiplicity limit. OCPN Draw registers two public SVG
tools; P1 registered three. Container IDs survive a UI rebuild but removal,
reload and restart make them non-contractual. `package_id + action_id` is the
only persistent identity.

Removal and visibility setters request a toolbar rebuild; insertion does not.
P1 proved a late public insertion can be made visible without restart by
calling `SetToolbarToolViz(new_id, true)`. This is technically valid but not a
clear contract. API 1.21 has no native tool enabled state. Stock customisation
explicitly ignores plugin tools, so host preferences cannot produce native
placement persistence.

Full answers to all 23 toolbar acceptance questions, including themes, DPI,
accessibility, multiple canvases, failure and update mapping, are in
`TOOLBAR-ACTION-FEASIBILITY.md`.

### UI and input

Plugins own normal wx dialogs/modeless windows, preferences and context menu
items. Context items have public dynamic visibility and grey/enabled state.
Plugins can load locale catalogues and receive color-scheme changes, but a
child translation remains mediated by the host. Mouse, keyboard, cursor and
canvas-selection APIs allow semantic hit testing without exposing wx/native
objects to a guest.

### Rendering

Modern API 1.21 supplies priority-aware software and GL multi-canvas callbacks,
coordinate transforms and refresh. Production Weather Routing, Radar and OCPN
Draw demonstrate sophisticated use. The safe model is one generation-tagged
immutable scene read by callbacks; all component and IPC work is completed
elsewhere. P3 exercised both stock paths with a guest scene.

### Navigation and charts

Position/heading/speed and cursor callbacks are supported. Public GUID and
value APIs enumerate/read/add/update/delete waypoints and routes; Weather
Routing is direct production evidence for route reads/creation.

The chart surface is qualitatively weaker. Metadata/directory XML and GSHHS
segment crossing exist, but no public immutable batched point/depth/hazard
query matches private chart internals. The current `charts.coverage` name is
incorrect because its implementation calls only GSHHS and sets
`charts_considered=1`. For preview use, rename it to advisory coastline
crossing and return unknown conservatively. Do not include private headers.

### Messaging, storage, network and concurrency

Plugin messages are synchronous broadcast strings. They are useful for GRIB,
climatology and OCPN Draw interoperability but are neither authenticated nor a
safe high-volume binary ABI. Keep typed component services internal and wrap
native-plugin JSON protocols explicitly.

Plugins can use config/private paths, native file dialogs and ordinary network
libraries/exported download helpers. The host must add scoped path grants,
quotas and network policy. API 1.21 has no cross-platform keychain abstraction.
Worker threads and child processes are normal plugin capabilities, with the
same OS/Flatpak permissions as OpenCPN. UI operations remain UI-thread only.

### Managed distribution

The outer host fits existing tarball/catalogue installation, target IDs,
updates and removal. Managed packages can include data, translations and
helpers. The installed data is read-only; child packages live in the private
root. A 22 MiB linked Linux probe demonstrates plausible but material runtime
size. DLL/shared-library visibility, Flatpak, hardened JIT, signing/notarisation
and target helper selection are release gates, not API blockers.

## Representative plugins

The source audit selected examples by capability:

- **OCPN Draw:** two toolbar actions, independent icons/state/cleanup, context
  UI and rendering;
- **Weather Routing:** closest navigation workload, modeless UI, route APIs,
  DC/GL overlays, workers and GRIB/climatology messages;
- **Radar:** high-rate multi-canvas rendering and network workers;
- **ShipDriver:** compact managed cross-platform plugin, SVG/PNG fallback,
  state/config/messages;
- **testplugin:** current template/API/messaging oracle.

They prove public-API reach but some use global state, raw IDs, synchronous
broadcasts or sleep-loop shutdown. Those patterns are not adopted. Details and
pinned paths are in `REPRESENTATIVE-PLUGINS.md`.

## Prototype results

| Prototype | Result | What it proved |
|---|---|---|
| P1 multi-package actions | Pass on stock 5.14 | Three distinct icons; late add/remove/reload; duplicate rejection; independent check; trap and shutdown cleanup; transient IDs |
| P2 Wasmtime lifecycle | Pass | Existing bridge/component can run inside a conventional plugin and cleanly disable/destroy |
| P3 retained overlays | Pass software and GL | Guest value scene renders through both public multi-canvas callbacks |
| P4 job/cancellation | Pass | Guest-requested worker did not block UI and joined in 30 ms during shutdown |
| P5 public path/navigation | Pass | Private root plus route/waypoint/chart-directory API resolved and executed with no private headers |
| P6 helper supervisor | Pass | Linux explicit argv/pipe/process-group cancellation stopped child and grandchild |
| P7 package management | 18/18 pass | Deterministic/signature/negative archive policy plus atomic update/rollback under temporary child root |

The stock executable was built from an unmodified 5.14 archive. GCC 16 needed
two warning-as-error demotions, not source changes. Full commands, logs and
limitations are in `PROTOTYPE-RESULTS.md`.

## Capability result and hard blockers

The 61-entry matrix yields 25 A, 23 B, 7 C, 3 D, no E, one F and two G.
Therefore the plugin model is not a façade over an essential hidden core
service. Its hard boundaries are:

1. **Native crash boundary.** Wasm traps are contained; the linked host/engine
   is not. Current release configuration uses Rust `panic=abort`.
2. **Nested identity.** Toolbar identity is feasible; native manager/catalogue
   identity is not.
3. **Toolbar quality gaps.** Dynamic add is workaround-shaped; no native
   enabled state/customised persistent child placement.
4. **Chart semantics.** Advisory coastline crossing is available; authoritative
   spatial chart/depth/hazard queries are not.
5. **Credentials/governance.** Secure persistent secrets, production trust
   roots, revocation, catalogue ownership and incident response need decisions.
6. **Platform containment.** Native helpers and JIT packaging cannot be claimed
   uniformly from one Linux experiment.

None prevents the stock-core vertical slice. The first, fifth and sixth prevent
an unqualified open third-party production ecosystem.

## Failure containment and process choice

### Embedded engine

Advantages: existing bridge works unchanged; lowest callback latency; easiest
render/action lifecycle; one managed artifact. Disadvantages: engine/Rust/C++
faults share OpenCPN, memory pressure is process-wide, and unload/symbol
compatibility is critical. Appropriate for signed preview/beta packages under
strict limits and target tests.

### External engine broker

Advantages: host survives broker abort/OOM/hang, stronger process quotas,
cleaner helper supervision/update. Disadvantages: framed authenticated IPC,
copy/shared-memory design, restart/generation logic, extra executables and
platform packaging. Rendering/actions still require a native in-process
adapter. Recommended before allowing unaffiliated signers.

### Hybrid

Best long-term execution design: a small adapter owns all OpenCPN objects;
broker owns engine/package execution/high-risk parsing; render callbacks read
already-published immutable scenes. The synchronous current C callback ABI is
kept behind an abstraction and later replaced by versioned bounded messages.

## Security and governance

Wasm is one control, not the whole sandbox. Production preserves:

- signed immutable packages and target-specific helper declarations;
- archive/path/count/size/decoded-resource validation;
- explicit permission deltas and revocation;
- memory/fuel/epoch/job/network/storage limits;
- no raw native pointers, callbacks, wx events or tool IDs;
- fail-closed unknown/invalid states;
- helper process-tree termination and honest per-platform guarantees;
- attributed logs with quotas/redaction;
- emergency runtime/package revocation and named incident owner.

The runtime creates an ecosystem inside an ecosystem. If no organisation owns
trust roots, catalogue review, runtime security releases, revocation and user
support, production must remain restricted to project-signed packages. The
technical host design cannot solve that governance gap.

## Platform conclusions

| Platform | Current conclusion |
|---|---|
| Linux x86-64 | Architecture mechanisms verified locally; first preview target |
| Linux ARM64 | Credible, pending executed native/helper conformance |
| Flatpak x86-64/ARM64 | Credible managed target; actual portal/network/spawn/containment tests required |
| Windows x86-64 | Credible; DLL/ABI/Job Object/signing tests required |
| macOS Intel/Apple Silicon | Credible; hardened JIT, helper signing and notarisation tests required |
| Android | Deferred/unsupported for this programme; no universal-portability claim |

## Architecture comparison

The fixed weighted matrix ranks standard plugin + generic API at 402/500,
hybrid broker + API at 364, thin broker at 355, pure plugin at 347, first-class
core at 329, external broker/current API at 334 and existing modified core at
280. Security-first weights make the hybrid broker win; a hard requirement for
native child manager entries makes first-class core integration win by
definition. See `ARCHITECTURE-OPTIONS.md`.

## Answers to the important distinctions

1. **Can Wasmtime run in a native plugin?** Yes; executed.
2. **Can iWeatherRouting work?** Principally yes; public navigation/UI/render/
   job precedents and bridge smoke pass. The recommended first slice proves it.
3. **Can iGRIB work?** Principally yes; its component ran in the stock host.
   Full helper/network/viewer packaging remains.
4. **Separate left-toolbar icons?** Yes; executed with three tools.
5. **Dynamic add/remove?** Yes on stock, but late add needs a rebuild workaround.
6. **Independent enable/check?** Check yes. Host dispatch enable yes. Native
   disabled visual/accessibility state no in API 1.21.
7. **All host services reproduced?** Useful services yes; chart safety,
   credentials and first-class identity are reduced/conditional.
8. **Managed catalogue?** Outer host yes; children require separate governance.
9. **Safe package download/execution?** Strong prototype controls exist; open
   third-party production also needs trust/revocation/incident ownership and a
   broker boundary.
10. **Are children standard plugins?** No. They are child extensions with
    separate toolbar presence.
11. **Community maintainability?** Credible if core-facing surface is small,
    runtime is separately owned and governance is explicit.
12. **Declared desktop targets?** Architecture credible; only Linux x86-64 is
    executed in this study.
13. **Every platform including Android?** No.

## Final migration direction

Preserve the reference, extract pure libraries, build the conventional shell,
productionise the action registry, add the engine/lifecycle executor, then UI,
overlays and the reduced iWeatherRouting vertical slice. Port environmental
services/iGRIB next, measure residual API gaps, package every desktop target,
add the broker security boundary and only then retire core wiring. The detailed
12-phase gates and rollback paths are in `MIGRATION-PLAN.md`.

## Conclusion

The best route is neither “keep it in core” nor “everything is already perfect
as a plugin.” A useful and honest portable runtime works on stock OpenCPN today,
including package-specific toolbar icons and dynamic cleanup. A small generic
action API converts the proven workaround into a durable beta contract. The
runtime host remains the right long-term adapter, while production execution
should move out of process as trust broadens. First-class child Plugin Manager
identity is optional future core integration, not evidence against this
architecture.
