# Study completion audit

This audit maps the original completion standard to committed, inspectable
evidence. “Verified” means the named evidence directly covers the requirement;
“qualified” means the requested study explicitly permits a practical/target
limitation and the report records it rather than generalising.

## Completion criteria

| Original criterion | Status | Authoritative evidence |
|---|:---:|---|
| Existing implementation traced service by service | Verified | `CURRENT-CORE-CHANGES.md`, `current-core-changes.json`, 88-path diff and 22 destination records covering stock hooks and added implementation groups |
| Current public plugin capabilities verified against source | Verified | Stock/upstream SHAs in `SOURCE-LEDGER.md`; API/lifecycle/UI/render/navigation/chart/message/distribution traces in `FEASIBILITY-STUDY.md` |
| Toolbar traced from public call to UI | Verified | `TOOLBAR-ACTION-FEASIBILITY.md` call graph and named functions in `ocpn_plugin_gui.cpp`, `pluginmanager.cpp`, `ocpn_frame.cpp` and `toolbar.cpp` |
| Representative plugins studied | Verified | Five pinned repositories and explicit per-plugin records in `REPRESENTATIVE-PLUGINS.md` |
| Multiple icons from one plugin tested | Verified | P1 stock-host log: manager, iGRIB and iWeatherRouting tools; production OCPN Draw corroboration |
| Dynamic addition/removal tested or disproved | Verified | P1 late UI-timer insertion, visibility-triggered rebuild, disable/trap removal and re-registration without restart |
| Stable identity/callback routing designed | Verified | Collision-tested `ActionRegistry`; `(package_id, action_id)` design; transient ID observations 1575→1577 and 1576→1578 |
| Cleanup/failure behaviour tested | Verified | P1 trap/disable/DeInit finished at zero actions; P2/P4 disable, job join and runtime destruction |
| Every portable host capability classified | Verified | 61-entry `capability-matrix.json`, covering every minimum capability named in the request; JSON parses and Markdown rationale mirrors it |
| Important uncertainty resolved with focused prototypes | Verified | P1–P7 in `PROTOTYPE-RESULTS.md`: toolbar, Wasmtime, DC/GL, jobs, navigation/paths, helpers and packages |
| Alternatives compared explicitly | Verified | Seven-option 12-criterion weighted matrix and three sensitivity sets in `ARCHITECTURE-OPTIONS.md` |
| Primary architecture recommended | Verified | Outcome B in executive/full reports, with stock preview and broker security evolution |
| Residual core changes defined/rule out | Verified | Preview/beta/first-class split and three generic conditional proposals in `MINIMAL-OPENCPN-CHANGES.md` |
| Realistic migration plan | Verified | Smallest vertical slice plus Phases 0–12, dependencies, tests, acceptance, rollback and risks in `MIGRATION-PLAN.md` |
| Platform/packaging implications documented | Verified | Target table in full/design reports; official installer paths and current Wasmtime platform constraints in source ledger |
| Security/governance implications documented | Verified | 34-entry risk register, trust/revocation/incident gates, in-process versus broker analysis |
| Toolbar identity distinguished from native plugin identity | Verified | Repeated explicit distinction in executive, toolbar, full and architecture reports |
| Required documents committed | Verified | All 13 named deliverables plus ledgers, representative audit and this completion audit are tracked on the study branch |

## Prototype requirement audit

| Requested prototype property | Evidence | Boundary |
|---|---|---|
| Runtime manager + iGRIB + iWeatherRouting tools | P1 stock run | Separate tools, one native owner |
| Distinct icons/tooltips | Three package-owned SVG fixtures and distinct insertion records | Pixel-perfect cross-platform visual validation remains a target gate |
| Independent callback routing | Public owner dispatch traced; reverse-map unit test; live stock-host autorun invoked recovered iWeatherRouting ID 1578 and toggled only its mapped action | Physical pointer automation was unnecessary because the core-to-owner dispatch path is directly traced |
| Checked/enabled state | Independent iGRIB check transition; dispatchability gate | Native grey/disabled state absent from API 1.21 |
| Clean remove/reload/rebuild | P1 late add, disable, trap, recover and shutdown sequence | Late add uses visibility setter to request rebuild |
| Restart identity | IDs proven transient by source/allocation and reload; logical key persists | Native placement is not persisted by child key |
| Minimal Wasmtime host | P2 stock load and bridge smoke | In-process native crash isolation is not claimed |
| Dynamic child lifecycle | P1/P2 combined state sequence | Static manifest actions are recommended over guest-only discovery |
| Overlay bridge | P3 software and actual OpenGL callbacks | No universal DPI/theme pixel claim |
| Job/cancellation | P4 worker cancellation/join plus bridge fuel/epoch tests | Arbitrary blocking native calls require a helper/broker deadline |
| Navigation capability | P5 stock public symbol calls plus production Weather Routing route operations | Empty isolated profile was deliberately not mutated |
| Native helper | P6 explicit argv, readiness pipe and Linux process-group tree kill | Other OS/Flatpak containment remains target-specific |
| Child package management | P7 18 policy tests, atomic replacement and rollback | Production roots/catalogue/revocation remain governance gates |

## Final validation evidence

The committed branch was exported to a fresh `/tmp` directory before final
validation so concurrent working files could not affect results:

- default and Wasmtime-enabled experiment configurations compiled;
- both configurations passed `action_registry` and `helper_supervisor` CTests
  (2/2 each);
- the C++ bridge smoke passed clean iGRIB and iWeatherRouting components;
- package and beta suites passed 30/30;
- all study JSON documents parsed with `jq`;
- all required document names existed and all local Markdown links resolved;
- `git show --check` reported no whitespace errors.

GUI evidence used a separately built unmodified stock 5.14 executable and
portable `/tmp` profile. It logged public dynamic toolbar transitions, linked
Wasmtime lifecycle, software and GL retained rendering, public path/navigation
enumeration and bounded shutdown. The user's separate OpenCPN 5.15 profile was
not accessed.

## Qualified conclusions, not omissions

- Light/dark/DPI/touch implementation paths were traced and SVG/raster support
  was exercised, but the compositor returned a black screenshot; the study
  therefore records target visual validation as a beta gate.
- Only Linux x86-64 runtime behaviour is verified. Other desktop targets are
  credible designs with mandatory executed conformance, not claimed support.
- Android is explicitly deferred rather than inferred from native-plugin or
  component portability.
- Chart coastline observations are advisory and are not described as
  authoritative safety/depth/hazard information.
- Portable children have distinct toolbar identity but are not first-class
  managed OpenCPN plugins.
