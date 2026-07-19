# Maintainer, governance and adoption impact

## Bottom line

Technical feasibility does not imply that OpenCPN should accept the continuing platform obligation. A production portable tier creates a public API, embedded execution engine, package ecosystem and security-response duty. It should ship only with named owners, budgets and a narrow first product. Otherwise retain it as an off-by-default experiment.

## Proposed ownership

| Area | Accountable owner | Required reviewers / duties |
|---|---|---|
| portable API/WIT and compatibility | OpenCPN portable API working group, ultimately core maintainers | domain owner for nav/chart/render/security; version/adapters/deprecation/conformance |
| core service implementations | relevant OpenCPN subsystem maintainers | thread/ownership semantics and internal reuse; no runtime leakage |
| Wasmtime adapter/dependency | named runtime maintainers (minimum two) | advisories, LTS/patch upgrades, build targets, fuzz/conformance, emergency disable |
| SDK/bindings/examples | plugin developer experience owner | generated releases, language matrices, docs and issue triage |
| package/catalogue trust | catalogue security team separate from publishers | TUF roots/delegations/revocation, key ceremonies, audit and mirrors |
| package publisher review | catalogue moderators | identity, permissions, licences/SBOM, helper scrutiny; no route-safety endorsement |
| security response | OpenCPN security team/on-call path | coordinated disclosure, runtime/package revocation, patched releases and communication |
| platform CI | release/platform maintainers | Windows, both macOS architectures, Linux/ARM/RPi and Flatpak coverage |

No single plugin author should be the sole owner of a core service or trust root. Changes to safety/environment semantics require documented domain review.

## Runtime maintenance decision

Wasmtime is recommended because it provides the Component Model, strong security focus, Windows/macOS/Linux and x86-64/AArch64 support, resource limiting and a documented release policy. It is Apache-2.0 licensed ([Wasmtime repository](https://github.com/bytecodealliance/wasmtime)).

Costs:

- a major release every month;
- normal releases supported for two months, LTS (multiples of 12) for 24 months;
- security fixes are backported to supported releases, general fixes only best effort;
- production OpenCPN must follow advisories and rebuild/distribute patched runtime versions;
- x86-64 host targets are Tier 1 while Linux/macOS AArch64 are Tier 2;
- Component/WASI/toolchains are still evolving toward Component Model 1.0;
- Rust integration or a recently expanded C API adds build expertise and CI time.

Policy proposal: prototype on current patched Wasmtime 46.x; production tracks one supported Wasmtime LTS with quarterly upgrade rehearsals and a maximum critical-advisory response target agreed by release maintainers (for example, triage in 24 hours and patched candidate in seven days). Never promise a time SLA until release infrastructure can meet it. Maintain a signed policy switch disabling portable instantiation if patch distribution is delayed.

OpenCPN should not fork Wasmtime. Keep a very narrow adapter, upstream issues, pin reproducibly and carry only short-lived reviewed patches.

## Mandatory or optional dependency

Keep it optional through at least one production cycle:

- source option `OCPN_ENABLE_PORTABLE_PLUGINS=OFF` by default during prototype;
- runtime preference false during experiment/beta;
- distro/Flatpak maintainers choose whether to include the target;
- flag-OFF build contains no Wasmtime, scanning, package UI or state migration;
- native plugin functionality does not depend on it.

If adoption justifies default inclusion later, that is a separate RFC with binary-size, startup, memory, packaging and security evidence. “Optional” does not excuse security maintenance in builds that ship it.

## Binary size, startup and release cadence

Do not set a size claim from Wasmtime download archives. Feature selection and static linking materially change the result. The prototype report must give per-platform:

- installed and compressed package delta;
- OpenCPN executable/shared-library delta;
- runtime compiler/cache dependency contribution;
- cold engine creation/component compile and warm cache startup;
- idle/per-instance/active RSS and cache storage;
- symbols/licence/SBOM/debug package impact.

Release maintainers set acceptable budgets after measurements. A runtime-only/AOT build can be smaller, but only host-created precompiled artifacts may be deserialised safely and a compilerless strategy complicates install/cache portability. Prefer lazy engine creation so users with the option enabled but no active portable plugin pay minimal startup cost.

Runtime security releases may require OpenCPN patch releases outside its normal cadence. Packaging teams need reproducible dependency updates on all target stores/channels. Catalogue package revocation should not require a new OpenCPN binary; runtime vulnerability remediation often does.

## API governance

The portable API is a long-term product surface. Proposed process:

1. WIT change begins with use case, threat/thread/data-volume analysis and conformance vectors.
2. Domain, API-compatibility, security and at least one plugin-developer reviewer approve.
3. Experimental interfaces are visibly namespaced/versioned and make no compatibility promise.
4. Stable major versions follow SemVer, coexist with adapters and have a minimum two-stable-release deprecation window.
5. No type may expose wx/internal/OS/GPU/C++ ABI concepts.
6. Batch shape is benchmarked on x86-64 and ARM64 before stabilisation.
7. Reference bindings/docs/tests release together with the host implementation.
8. Safety-related wording/status changes receive navigation/chart domain review and remain advisory.

An API compatibility dashboard lists which OpenCPN releases implement each interface/feature and when adapters expire. Capability discovery is preferred to version tests.

## Catalogue and signing governance

Separate duties:

- offline threshold root authorises catalogue roles and publisher delegations;
- timestamp automation has a short-lived online key;
- package publishers sign only their namespace;
- catalogue moderators authorise/revoke identities and review elevated permissions/helpers;
- security responders can publish emergency revocation/minimum-safe-version metadata through audited process.

Document key generation/storage/rotation/recovery, quorum, transparency monitoring, incident drills and succession. A catalogue listing means identity/policy review, not OpenCPN certification of computations or data. Offline installs clearly show reduced revocation assurance.

TUF directly addresses repository/key compromise, rollback and freeze threats and should be reused rather than partially recreated ([TUF overview](https://theupdateframework.io/docs/overview/)). Sigstore may add identity/provenance/transparency, but the project must operate or depend on its roots/log availability and monitor identities.

## Dependency and licence burden

For each shipped dependency record exact version/source/hash, licence, transitive native libraries, build features, CVE/advisory source, update owner and platform status in an SBOM. Initial candidates:

- Wasmtime/Cranelift and Rust crates: Apache-2.0 family; verify every transitive crate;
- deterministic ZIP and JSON/canonicalisation/signature/TUF libraries: select only after security/licence review;
- optional Cap'n Proto or other helper protocol: add only after benchmark/maintenance decision;
- platform credential/portal APIs;
- helper-private ecCodes, NetCDF, PROJ, curl, archive/compression and data licences.

OpenCPN is GPL-2.0; maintainers must perform a compatibility review rather than assuming a dependency's permissive headline covers all bundled artifacts/data. Helper and guest packages carry their own licence notices/SBOM and catalogue policy.

## Documentation and support

Minimum maintained documentation:

- architecture/security/package/API lifecycle specifications;
- each WIT interface, thread/cancel/error/permission and examples;
- SDK install/build/package/sign/debug/conformance guides;
- platform/Flatpak limitations and helper availability;
- migration decision guide for native authors;
- user permission/trust/update/revocation and troubleshooting pages;
- security reporting and supported runtime/API matrix.

Support load will include guest toolchain problems, package/signing errors, API compatibility, quotas/performance, platform portals and runtime traps. Stable error codes, local conformance and privacy-scrubbed diagnostics are essential to avoid making core maintainers debug every language build.

## CI and hardware burden

Every change keeps flag-OFF native builds/tests. Flag-ON tiers:

- per-PR: Linux x86-64 service/runtime/conformance, package-negative corpus and native coexistence;
- regular: Windows x86-64, macOS x86-64/ARM64, Linux ARM64, Flatpak x86-64/ARM64;
- scheduled/release hardware: Raspberry Pi 64-bit, full performance/failure cycles and code-signing/notarisation/install paths;
- security dependency update: full component conformance, package signature/update and trap/DoS suite before expedited release.

Third-party developers run the same conformance binary/fixtures. A target is not “supported” because Wasmtime builds there; OpenCPN package, UI, portal, credential, renderer and workload tests must pass.

## Smallest production-worthy release

Meaningful but supportable scope:

- signed/revocable **Wasm-only** `.ocpnp` packages;
- Wasmtime supported LTS and owned update process;
- lifecycle, negotiation, permissions, settings, private/cache/temp and selected-file storage;
- host HTTP with declared domains and credential injection;
- navigation snapshot/read and route creation with clear confirmation/identity;
- cancellable bounded jobs with progress/diagnostics;
- basic host-rendered actions/forms/notifications;
- retained line/polyline/route/corridor overlay without arbitrary shaders;
- environment dataset metadata and batch sampling consumption;
- Rust plus one additional conformance-passing guest language;
- all named target CI, catalogue trust/update/rollback and independent SDK conformance.

Exclude from the first production promise: native helpers, ecCodes/NetCDF generation, raw sockets, WebView, shared memory, arbitrary plugin-to-plugin publication, custom charts, broad chart-depth/safety authority, native UI and direct graphics. Chart `validate-segments` may remain experimental until semantics and fixtures are accepted even if the routing reference uses it.

## Adoption decision gates

### Approve vertical prototype if

- maintainers accept the hybrid/service direction and Rust/Wasmtime experiment;
- two people own runtime/security work for the prototype;
- flag isolation and no native-plugin changes are non-negotiable;
- Stage 2 scope remains the 13-point slice.

### Approve beta if

- trap/timeout/cancel/shutdown and platform tests pass;
- package/update threat controls are implemented, not merely documented;
- xGRIB/Weather Routing reference tests validate batch shapes;
- linked size/startup/memory and ARM/Flatpak results are accepted;
- C/C++ and at least one high-level tooling decision is evidence-based;
- API/catalogue/security owners and deprecation policy are named.

### Approve production if

- smallest product scope above is complete with conformance and docs;
- a supported Wasmtime LTS has passed all targets;
- incident/revocation/runtime upgrade drills succeed;
- release channels can deliver critical patches;
- ongoing staffing and CI budget are committed.

### Keep experimental or stop if

- ownership rests on one person;
- core services become runtime-specific or invasive before the slice works;
- flag-OFF behavior cannot remain unchanged;
- runtime patch cadence exceeds distribution capability;
- ARM64/Flatpak or workload performance cannot meet bounded behavior;
- the project must expose ambient filesystem/network/native execution to be useful.

Extracted value-based services and headless fixtures remain worthwhile even if the portable runtime is not adopted.
