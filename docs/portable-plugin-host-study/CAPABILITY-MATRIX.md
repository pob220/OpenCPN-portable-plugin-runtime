# Portable host capability matrix

## Scale and overall result

The classification is architectural, not a maturity score. **A** means a clean
public-plugin implementation; **B**, plugin-owned with redesign; **C**, useful
but reduced; **D**, a small generic public API addition; **E**, a thin
privileged core broker; **F**, first-class core integration; and **G**, remove
or defer. The exact 61-entry dataset, evidence strings and qualifications are
also published in [`capability-matrix.json`](capability-matrix.json).

The distribution is A=25, B=23, C=7, D=3, E=0, F=1 and G=2. This is strong
evidence for a plugin host: none of the useful preview services needs a thin
core broker. It is equally important that the three D services and the one F
service are not hidden by that headline.

## Runtime, packages and security

| Capability | Class | Decisive evidence and rationale |
|---|:---:|---|
| Wasmtime ownership | A | `portable-runtime/bridge` has no OpenCPN private dependency; P2 linked it into and ran it from the stock host. |
| Component loading | A | P2 loaded the existing wasm32-wasip2 iGRIB artifact by validated path. |
| Component lifecycle | A | P2 completed initialize, enable, action, disable and destroy in stock 5.14. |
| Package discovery | A | Host-private root is public through `GetpPrivateApplicationDataLocation`; discovery is ordinary filesystem work. |
| Package install | B | `install_package.py` already stages and atomically installs, but storage policy and UI must become host-owned. |
| Package verification | A | 18 package-tool tests passed checksum/signature/path/archive/target negative cases. |
| Package update | B | Atomic replacement and rollback pass; catalogue, consent and failed-update UX are new host responsibilities. |
| Package enable/disable | A | P1 independently retracted and restored child actions while the outer plugin remained loaded. |
| Permissions | B | Existing checks are host-owned; persistent consent, grant revisions and revocation need redesign. |
| User consent | B | A wx plugin can render it, but OpenCPN's Plugin Manager cannot present child grants. |
| Private storage | A | P5 resolved the portable private root; namespace packages below a runtime-owned directory. |
| File import/export | B | Public/wx dialogs can grant explicit files; convert ambient file access to scoped host value operations. |
| HTTP | B | Current constrained download-to-private callback moves, but proxy/TLS/redirect/cancellation policy must be completed. |
| Credentials | C | Session memory/platform keychains are useful; API 1.21 provides no portable secure credential broker. |
| Resource limits | A | Bridge uses `StoreLimits`, fuel and epoch deadlines and its smoke tests exercise bounded calls/traps. |
| Wasm trap containment | A | Deliberate guest traps return through the C bridge; the action registry can retract failed contributions. |
| Native-host crash containment | C | The linked Rust profile is `panic=abort`; only a separate broker protects OpenCPN from an engine/host abort. |
| Signing | B | Ed25519 verification works; production roots, rotation, provenance and helper/platform signatures are governance work. |
| Revocation | D | A signed list is implementable, but trusted distribution/emergency policy needs an accepted generic update contract. |
| Telemetry | G | No required contract exists; defer it and keep local opt-in diagnostics. |

## Actions and UI

| Capability | Class | Decisive evidence and rationale |
|---|:---:|---|
| Package toolbar icons | A | P1 created distinct manager, iGRIB and iWeatherRouting SVG tools on stock OpenCPN. |
| Multiple icons per host | A | P1 and production OCPN Draw prove multiple public tools with one native owner. |
| Dynamic registration | B | Late add works only after `SetToolbarToolViz` forces the rebuild omitted by insertion. |
| Stable action identity | B | P1 maps `(package_id, action_id)` to transient integers and rejects both logical and native collisions. |
| Toolbar persistence/customisation | C | Host state can persist; `toolbar.cpp` expressly omits plugin tools from stock customisation and placement persistence. |
| Themes and scaling | B | SVG normal/rollover/toggled APIs and core scaling apply; disabled artwork and target visual validation remain. |
| Toolbar cleanup | A | P1 passed disable, trap, recovery and DeInit removal, finishing at registry size zero. |
| Menus/context actions | A | Public add/remove/visible/grey context-item APIs have full dynamic state support. |
| Preferences | B | Host and child settings fit one runtime manager; children cannot get independent standard manager panels. |
| Dialogs/modeless UI | A | Weather Routing and OCPN Draw are production precedents for complex plugin-owned UI. |
| Declarative UI | B | Existing bounded wx renderer moves, but its large core-facing host must be decomposed and schema/a11y completed. |
| Translations | B | `AddLocaleCatalog` and host key resolution work; child fallback/trust and standard identity do not come for free. |

## Rendering, navigation, charts and data services

| Capability | Class | Decisive evidence and rationale |
|---|:---:|---|
| Retained overlays | A | P3 rendered one guest-produced scene through both stock software and GL multi-canvas callbacks. |
| Hit testing/input | B | Public cursor/mouse/keyboard callbacks suffice; the host must expose semantic, bounded events rather than wx objects. |
| Navigation snapshots | A | `SetPositionFixEx`/cursor callbacks are public; P5 confirmed stock API linkage and enumeration. |
| Routes | A | Public GUID/value APIs and production Weather Routing provide complete read precedent. |
| Waypoints | A | Public GUID/value APIs support read/add/update/delete without private pointers. |
| Route creation | A | `AddPlugInRouteExV2`/update and Weather Routing establish a supported path. |
| Chart coverage | C | Metadata and coastline crossing are available, but the current WIT service actually calls GSHHS and is not chart coverage. |
| Land/hazard classification | D | Public GSHHS is an advisory crossing helper; generic bounded chart/depth/hazard queries are needed for stronger semantics. |
| Depth/chart-object queries | D | API 1.21 lacks a safe immutable batch surface; private chart headers are explicitly rejected. |
| Environmental providers | B | Provider/cache/helper work is plugin-owned, but `PortableEnvironmentHost` must be split into bounded modules. |
| Environmental consumers | A | Typed internal routing passed the bridge smoke test and requires no OpenCPN hook. |
| Component-to-component services | B | SemVer and typed values move; discovery, failure and backpressure require a formal host contract. |
| Native-plugin communication | C | Synchronous broadcast JSON is useful, but has no authenticated addressing, ownership or safe binary lifetime. |

The two chart D entries describe one possible generic API family, not a demand
for two independent core patches. For the preview, rename the existing
`charts.coverage` result to its actual advisory coastline-crossing meaning and
defer authoritative depth/hazard claims.

## Jobs, helpers and diagnostics

| Capability | Class | Decisive evidence and rationale |
|---|:---:|---|
| Background jobs | A | P4 started a worker from a guest request without blocking the UI/render callback. |
| Cancellation | A | P4 cancelled/joined in 30 ms during stock-host shutdown; bridge smoke also exercises route cancellation. |
| Native helpers | B | Helpers are valid managed-plugin payloads, but target paths/protocol/version selection need repackaging. |
| Process containment | C | P6 killed a Linux child and grandchild process group; Windows/macOS/Flatpak guarantees differ. |
| Logging | A | P1–P5 used attributable package/host log prefixes; production needs rate/size/redaction limits. |
| Diagnostics | A | Health/version/job/helper state is host-owned; only native crashes lack automatic child attribution. |

## Distribution, identity and platforms

| Capability | Class | Decisive evidence and rationale |
|---|:---:|---|
| Plugin Manager presentation | C | The runtime is a normal managed plugin; toolbar children and runtime-manager entries are not native manager entries. |
| Managed catalogue integration | B | Standard host tarballs fit the catalogue; child catalogue governance is a second ecosystem decision. |
| Bundled runtime/libraries | B | P2 produced a 22 MiB Linux module; target loader, symbol, signing and security-update work remains. |
| Linux x86-64 | A | P1–P6 exercised the central mechanisms against an unmodified stock executable. |
| Linux ARM64 | B | Wasmtime/managed patterns support it; target build and runtime conformance are still mandatory. |
| Flatpak | B | Managed helpers/data have defined sandbox paths; portals, downloads, spawning and nested containment need SDK evidence. |
| Windows x86-64 | B | Templates and Job Objects provide a design, not executed evidence; DLL/signing/ABI gates remain. |
| macOS Intel/Apple Silicon | B | Engine architectures are viable; hardened JIT, notarisation and helper signing need execution evidence. |
| Android | G | Current build rejects it, 32-bit lacks a Wasmtime compiler backend, and mobile helper/plugin assumptions are unresolved. |
| First-class child identity | F | Only OpenCPN core can make each child independently installed, listed, enabled, permissioned and crash-attributed by its native manager. |

## Acceptance interpretation

A useful desktop technology preview needs only A/B services plus the explicit
C approximations. A broadly usable beta should add the generic toolbar action
API and a secure credential decision; it can still defer authoritative chart
queries. First-class production identity is an independent F requirement: it
does not invalidate the plugin host, but it cannot be achieved by painting
more toolbar icons.
