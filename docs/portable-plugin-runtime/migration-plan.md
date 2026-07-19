# Coexistence and migration plan

## Principle

Portable plugins are a second tier. Migration is optional and suitability-based, not a conversion deadline. Native plugins remain the correct choice for custom chart engines, native hardware/proprietary SDKs, unrestricted native UI/rendering and other capabilities deliberately absent from the portable tier.

No existing plugin changes in the prototype. The native loader, API 1.21 negotiation, target tarballs, native catalogue records and Plugin Manager operations continue.

## Two parallel stacks

| Concern | Native tier | Portable tier |
|---|---|---|
| executable | shared library and C++ object | signed WebAssembly Component; optional helper exception |
| loader | existing `PluginLoader`/`PlugInContainer` | `PortablePluginSupervisor` |
| metadata | target-specific native catalogue/tarball | signed portable catalogue and `.ocpnp` |
| API | `ocpn_plugin.h`, wx/C++ ABI | versioned WIT services/value types |
| UI/overlay | native wx/DC/GL callbacks | host declarative UI/retained scene |
| storage/network | native process authority/API conventions | explicit capability services |
| failure | native fault can crash core | component trap contained; helper process supervised |

Plugin Manager may present one list, but each row retains a `native` or `portable` kind and delegates actions to its own registry. Package ids cannot impersonate an installed native common name; UI shows publisher, tier and trust.

## Service-first extraction

The durable migration target is the OpenCPN service layer, not Wasmtime. Implement service interfaces as GUI-neutral C++ abstractions with value DTOs and executors, then add:

1. WIT host adapters for portable components;
2. internal callers/headless fakes;
3. optional generated/handwritten native C++ convenience wrappers.

A native wrapper is an in-process call and need not serialize through WIT, but must preserve the public service semantics: immutable values/UUIDs/revisions, explicit errors, async/cancel and no internal pointers. This lets native plugins migrate one concept at a time without changing loader ABI.

## Existing concept disposition

### Wrap safely through value services

- navigation snapshot and selected decoded message subscriptions;
- route/waypoint/track list/get/create/update/apply and GPX import/export;
- toolbar/menu actions, notifications and settings forms;
- localisation resources and ordinary namespaced settings;
- selected-file open/save;
- retained route/line/polygon overlay concepts;
- environment provider/consumer behavior after a typed adapter is agreed;
- progress/cancellation/diagnostic jobs.

### Replace at the public boundary

- raw `PlugIn_Route`, waypoint/track pointers and wx collections → UUID/revision records and transactions;
- `wxFileConfig*` → settings plus credentials brokers;
- chart database XML/directories/native chart objects → coverage/safety query services;
- `SetPluginMessage(message_id, body)` ecosystems → WIT provider/consumer services;
- `wxDC`/`wxGLContext` render callbacks → retained scene;
- synchronous callbacks and plugin-owned worker interaction with GUI → async requests/events;
- direct curl/files/sockets → host HTTP and resource handles.

Selected legacy string protocols may have a native bridge process/adapter during transition. The bridge validates a known message schema and identifies the provider; generic string broadcast is never exposed as the primary portable service.

### Remain native-only initially

- custom/proprietary chart classes and direct chart-render implementations;
- direct GPU/native window/control/context access;
- device drivers/raw OS handles and arbitrary sockets;
- in-process SDKs that require a platform C/C++ ABI;
- plugins which intentionally modify OpenCPN internals outside approved services.

## Developer SDK

Publish one versioned SDK repository generated from authoritative WIT packages. It contains:

- WIT source, semantic/version policy and rendered reference;
- `ocpn-component` build/package/sign/inspect/test CLI;
- generated guest bindings and idiomatic facade libraries;
- local conformance host with deterministic navigation/environment/chart fixtures;
- package schema/canonicalisation/signature and malicious-input vectors;
- structured logging/tracing helpers and symbol/source-map guidance;
- CI recipes producing a single Wasm-only `.ocpnp`;
- permission and compatibility linter;
- reference packages and headless benchmark scenarios.

Required examples before production advertising:

- **Rust**: primary full lifecycle, async/job, error and retained-scene example;
- **C**: generated WIT bindings plus a small component proving memory/ownership/error behavior;
- **C++**: C bindings wrapped by value/RAII conveniences without exceptions crossing WIT;
- **higher-level language**: JavaScript/TypeScript or Python only after the chosen toolchain passes API/async/size/startup conformance. JavaScript is likely the first usability candidate; do not promise it before current `jco/componentize-js` behavior is measured.

C/C++ component tooling currently lacks a single integrated path, so its prototype is a feasibility gate, not release-marketing text ([Component Model C/C++ tooling](https://component-model.bytecodealliance.org/language-support/c.html)). Python tooling bundles CPython and can create large self-contained components; package size/startup remain explicit gates ([componentize-py](https://github.com/bytecodealliance/componentize-py)).

## Debugging workflow

Developer mode shows component/package digest, negotiated world/interfaces/features, granted services, resource/quota counters, lifecycle state, last trap with symbolic Wasm stack where available, host-call trace with timing/size, job/cancellation state and scene/UI registry contents. Secrets and precise navigation data are redacted unless separately opted in.

The CLI validates and locally instantiates packages against fakes, injects trap/timeout/provider-loss cases, records/replays typed service fixtures and verifies clean resource drops. OpenCPN can export a privacy-scrubbed diagnostic bundle. Native helpers use stderr JSON/trace pipe, crash metadata and platform debug symbols; production never exposes an interactive helper shell.

Debug behavior may differ by platform—Wasmtime lists DWARF support as best-effort—so stable error codes and trace correlation are mandatory even when source stepping is unavailable.

## Phased adoption

### Phase A — experimental vertical slice

One in-tree Rust test component only. No catalogue migration, no third-party promise. Validate discovery, manifest, WIT negotiation, consent, one action, position, setting, cancellable job, overlay, trap and disable. Flag OFF remains default.

### Phase B — reference workloads

Add the two small architectural reference components, not xGRIB/Weather Routing ports. Environmental example: host HTTP, progress/cancel, private storage, selected output, metadata, simple overlay and structured provider error. Routing example: environment sample batch, chart-safety batch, cancellable compute, route/corridor scenes and structured result.

### Phase C — SDK and native wrappers

Stabilise only services proven by B. Publish Rust/C/C++ bindings and conformance. Offer native wrappers for these services while retaining old API calls. Adapt one known `GRIB_*` exchange through a typed environmental bridge as an experiment; do not bridge arbitrary broadcasts.

### Phase D — opt-in beta catalogue

Separate signed beta root/channel, reviewed publishers, explicit permission UI and automatic revocation. Platform CI/conformance and performance gates must pass. Runtime remains optional and can be disabled remotely by signed policy.

### Phase E — smallest production release

Signed Wasm-only packages and the narrow service set listed in the RFC. Keep helpers, WebView, raw sockets, shared memory and broad third-party service publication experimental. A native plugin may publish a typed provider only through a reviewed core adapter.

### Phase F — plugin-by-plugin decisions

Use a capability worksheet:

1. Can the computation run in a component with acceptable performance?
2. Are all host needs covered by stable least-privilege services?
3. Can UI be expressed in the supported declarative subset?
4. Does it require a native helper, and does that still reduce the real build matrix?
5. Are state, licences, tests and security ownership acceptable?

If not, stay native. Partial migration is useful: a native plugin may call new core batch services before its UI/compute becomes portable.

## xGRIB migration path

1. Define environment metadata, dataset resource and sample-batch conformance from viewer/routing needs.
2. Wrap the existing generator process behind the signed helper protocol without changing its algorithms; remove ambient environment/files/network in favour of granted resources.
3. Build the small environmental reference component and typed provider registry.
4. Move portable provider orchestration/timeline state into a component only if UI and performance tests pass.
5. Keep ecCodes/NetCDF/PROJ generator helper until a Wasm build wins on maintenance, robustness, size and throughput.
6. A full viewer port is a later product decision; the native xGRIB plugin remains supported throughout.

## Weather Routing migration path

1. Finish separating engine DTOs from wx types using headless scenario fixtures.
2. Extract environmental batch and chart prepared-cache/segment-batch services.
3. Compile a small route engine/reference algorithm as a component and prove cancellation/results.
4. Add retained route/alternative/corridor scene transfer and navigation route creation.
5. Compare realistic scenarios to the native engine; only then consider moving production computation.
6. Native Weather Routing can consume the same services through C++ wrappers independently of a full component port.

## Compatibility and deprecation

- Portable WIT packages use SemVer and explicit ranges; multiple major adapters coexist.
- Deprecate a stable major for at least two stable OpenCPN release cycles and publish migration tooling/fixtures before removal.
- A package compiled against an old supported world continues through an adapter; unsupported required major fails before instantiation with a clear error.
- Optional interfaces/features are discovered, never assumed from OpenCPN version.
- State schema migration is plugin-owned, transactional and package-declared; public service migration is host-owned.
- Native API deprecation/removal is outside this proposal. New native service wrappers may be preferred for new work, but old plugins remain functional.

## Central native build service

Maintain and improve template CI as the fallback for native-only plugins and target-specific helper payloads. Centralisation can provide reproducible containers, SBOM/provenance, common signing and conformance, but it still emits a matrix. Do not label a multi-binary archive “build once.” Helper CI should share package identity/signing policy and be reproducible where practical.

## Success and stop criteria

Success means at least one nontrivial Wasm-only reference package passes every target unchanged; a second workload proves batch chart/environment services; failure containment and lifecycle are reliable; maintainers own the runtime/API/security process; and third parties can reproduce conformance.

Stop or narrow the project if flag-OFF behavior regresses, runtime/security updates cannot meet OpenCPN cadence, ARM64/Flatpak cannot meet gates, host-service extraction becomes invasive before the slice works, language tooling cannot produce conforming components, or the smallest production surface still exceeds available maintainership. In that case keep extracted services/native wrappers and use central builds/helpers without promising a portable tier.
