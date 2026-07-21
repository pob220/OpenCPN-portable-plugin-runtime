# OpenCPN portable plugin package format

## Decision

Use `.ocpnp` (**OpenCPN Portable Plugin**) as a deterministic ZIP container. A Wasm-only `.ocpnp` is the same byte sequence on every supported host. Optional native helpers may be present under target-qualified paths; the archive remains a single file, but capabilities then differ by target and the plugin is not strictly Wasm-only.

Package format `1` is independent of OpenCPN version, portable API version, component encoding and plugin version.

Prototype implementation note: the current deterministic producer uses
`format_version`, `component`, a complete `checksums.sha256` inventory and an
Ed25519 `signature.json` which signs the exact checksum bytes. The installer
requires a configured key id, keeps package files immutable, allows executable
bits only below `helpers/`, and supports verified atomic `--replace` with a
retained rollback directory. This deliberately small development profile is
implemented and tested; the richer canonical-manifest/TUF/Sigstore catalogue
profile below remains the production proposal.

## Layout

```text
org.example.weather-2.3.1.ocpnp
├── manifest.json                 canonical RFC 8785 JSON
├── component/plugin.wasm        WebAssembly Component, not core module
├── interfaces/                  exact WIT sources used to build (diagnostic)
│   └── world.wit
├── resources/
│   ├── icons/...
│   ├── ui/...                   optional declarative UI documents
│   └── data/...
├── i18n/<locale>.mo
├── migrations/<from>-<to>.wasm optional restricted migration component
├── helpers/<target>/<helper-id>/
│   ├── helper-manifest.json
│   ├── executable
│   └── runtime/...              declared private libraries/data only
├── licenses/...
├── sbom/spdx.json
├── checksums.sha256
└── signatures/package.sigstore  detached verification bundle/signature
```

ZIP entry names are UTF-8, forward-slash, NFC-normalised relative paths. Reject absolute paths, `..`, empty components, backslashes, NUL/control characters, duplicate or case-fold-colliding names, symlinks, hard links, devices and encrypted entries. Manifest limits set upper bounds below host policy; host policy always wins. Signing input uses canonical manifest bytes plus a canonical ordered `(path, size, SHA-256)` list. ZIP metadata/order/compression do not affect the content identity but reproducible producers use fixed timestamps/order/modes.

The prototype installer admits at most a 512 MiB compressed archive, 1 GiB
uncompressed content, 32,768 archive entries, a 1 MiB manifest and a 200:1
per-entry compression ratio.  The relatively generous entry limit is required
because the audited ecCodes definition data contains thousands of small files,
and a complete package can carry helpers for seven target environments.  These
are **plugin installation limits only**: they do not limit the byte size, grid
extent, point count or message count of a user-selected or downloaded GRIB.
Environmental datasets remain outside the immutable package and are handled by
the separately supervised, streaming and batched environment service.

The WIT copies are documentation; the component's embedded type information is authoritative and must match the declared world. The host never executes a core Wasm module disguised as a component.

## Manifest essentials

Illustrative shape (field names are a proposal):

```json
{
  "package_format": 1,
  "id": "org.example.weather",
  "name": "Example Weather",
  "version": "2.3.1",
  "publisher": "org.example",
  "entry": "component/plugin.wasm",
  "runtime": {
    "portable_runtime": ">=0.1.0 <0.2.0",
    "component_profile": "opencpn-component-p3-2026-06",
    "wasi": "0.3.0"
  },
  "opencpn_api": ">=1.0.0 <2.0.0",
  "world": "opencpn:plugin/runtime@1",
  "requires": [
    {"interface": "opencpn:navigation", "range": ">=1.0 <2.0", "features": ["position"]},
    {"interface": "opencpn:jobs", "range": ">=1.0 <2.0"}
  ],
  "optional": [
    {"interface": "opencpn:charts-safety", "range": ">=1.0 <2.0"}
  ],
  "permissions": [
    {"name": "navigation.read", "reason": "Seed route calculations"},
    {"name": "overlay.submit", "limits": {"max_scene_bytes": 4194304}},
    {"name": "network.http", "domains": ["weather.example"], "methods": ["GET"]}
  ],
  "provides": [{"interface": "org.example:environment-provider", "version": "1.1.0"}],
  "resources": {"ui": "resources/ui/main.json", "icon": "resources/icons/plugin.svg"},
  "state_schema": 3,
  "migrations": [{"from": 2, "to": 3, "component": "migrations/2-3.wasm"}],
  "helpers": [],
  "files": [{"path": "component/plugin.wasm", "size": 1234, "sha256": "..."}],
  "licenses": ["Apache-2.0"]
}
```

Requirements:

- `id` is reverse-DNS, lowercase ASCII and immutable; it namespaces storage/settings/services.
- `version` is SemVer 2.0.0. Build metadata does not determine update precedence.
- `portable_runtime` versions OpenCPN's loader/supervisor contract, not Wasmtime's library release; `component_profile` pins the accepted component encoding/async ABI and `wasi` pins imported WASI semantics. This avoids pretending the still-pre-1.0 Component Model has WASI's version number.
- ranges use one documented grammar; prereleases require explicit opt-in.
- unknown fields are retained for signatures but ignored only when schema declares them optional. Unknown required features reject compatibility.
- permissions contain machine scope and localisable reason key. A manifest cannot grant itself a permission or enlarge host quotas.
- files list every non-signature regular file exactly once. Checksums file is a convenient duplicate view; signed manifest file list is authoritative.
- component/resources/license/SBOM size and count limits are policy inputs, not suggestions.

### Optional helper declaration

```json
{
  "id": "environmental-grib",
  "optional": true,
  "targets": {
    "linux-gnu-aarch64": "helpers/linux-gnu-aarch64/environmental-grib/helper-manifest.json",
    "flatpak-aarch64": "helpers/flatpak-aarch64/environmental-grib/helper-manifest.json"
  },
  "protocol": "org.opencpn.helper.environmental-grib@1",
  "capabilities": ["environment.decode", "environment.write"],
  "fallback": "disable-generation"
}
```

Each helper manifest lists executable/private libraries/data, hashes, supported protocol, resource budgets and requested broker capabilities. Target selection is exact—OS, ABI/libc or Flatpak runtime and architecture—not filename guessing. Every payload shares the package signature. Platform code-signing/notarisation remains required where the OS demands it.

## Installation and state

1. Download to a private temporary file with byte/time limit.
2. Validate outer ZIP structure and read bounded `manifest.json` only.
3. Verify catalogue expected id/version/digest/size and package signature/revocation.
4. Validate all entries and hashes while extracting to a new immutable version slot.
5. Validate component/world/API and target helper metadata without executing it.
6. Present compatibility and permission delta; obtain consent.
7. Atomically mark the slot installed. Enabling/instantiation is a separate action.

Use dedicated per-user `portable-plugins/packages/<id>/<version>/<digest>/` and state roots selected through OpenCPN platform paths. Never extract to native plugin library directories. Component compilation cache keys include package digest, component type/WASI profile, runtime build id, host target, compiler settings and portable API adapter revision. A mismatch discards the cache. Only the trusted host creates cache/AOT artifacts; guest-provided precompiled native images are never deserialised.

Settings/state are outside the immutable package and keyed by package id plus state schema. Update runs a separately limited migration transaction on a snapshot, then switches package and state pointers together. Failed migration leaves the old version enabled. Cache is disposable and never migrated as authoritative state.

## Catalogue metadata and trust

Portable catalogue targets contain:

- package identity, display metadata and version;
- exact `.ocpnp` digest/size/download mirrors;
- allowed publisher identity/signing policy;
- required package/API/runtime profile summary;
- permission summary and helper target availability;
- release channel, minimum safe version, withdrawn/revoked status;
- source, licence and SBOM/provenance links.

Use TUF-compatible root/targets/snapshot/timestamp roles. Publisher delegation can constrain identities to package-id namespaces. Root keys are offline and threshold-signed; timestamp is online/short-lived; clients persist last-seen versions and reject rollback/freeze/expired metadata. Catalogue compromise alone cannot substitute bytes signed by an unauthorised publisher, and publisher compromise can be revoked by catalogue/root policy.

A package signature may be an OpenCPN-trusted Ed25519/X.509 form or a Sigstore bundle binding an approved OIDC/CI identity. The production RFC must select one verification profile and libraries after licence/security review. Sigstore's bundle supports inclusion/timestamp material and offline verification ([Sigstore bundle](https://docs.sigstore.dev/about/bundle/)); TUF remains the update authorisation/revocation layer.

## Update, rollback and revocation

- Install versions side by side; do not overwrite the active slot.
- Verify/migrate/start the new version transactionally, then switch active pointer.
- Retain at least one known-good package/state pair within quota.
- Automatic rollback is allowed only for startup/compatibility failure before new state is committed. Later rollback is explicit and restores the matching state snapshot.
- Never roll back below signed `minimum_safe_version` without developer override and warning.
- Revoked packages do not start; a running instance is suspended and user informed according to severity. Critical runtime-profile revocation can disable all portable execution.
- Offline clients use cached, unexpired trusted metadata. Expired metadata permits already-installed non-revoked packages according to an explicit grace policy, but blocks catalogue updates; high-severity policy may fail closed.

## Offline and developer installation

Offline install verifies the embedded signature against locally trusted roots and shows when catalogue freshness/revocation could not be checked. The user sees signer, digest, permissions and portability/helper status. It never treats “valid cryptography” as catalogue approval.

Developer mode accepts a package signed by a configured developer key or a one-session test key, stores it under a separate channel/root and displays a persistent untrusted badge. It permits verbose diagnostics and hot reinstall but not broader services. Unsigned packages are confined to test builds and require a command-line/runtime developer switch; production builds may omit that path.

## Compatibility errors

Errors are stable codes with details:

- `package-format-unsupported`;
- `manifest-invalid` / `archive-policy-violation` / `digest-mismatch`;
- `signature-untrusted` / `signer-not-authorized` / `revoked` / `metadata-expired`;
- `component-invalid` / `world-mismatch` / `wasi-profile-unsupported`;
- `portable-api-no-match` / `required-interface-missing` / `required-feature-missing`;
- `permission-denied` / `quota-incompatible`;
- `helper-target-unavailable` / `helper-signature-invalid` / `helper-protocol-no-match`;
- `state-migration-unavailable` / `state-migration-failed`.

The Plugin Manager shows the corrective action and diagnostic id. It never attempts native loading, substitutes another helper or relaxes permissions as a fallback.

## Package-format conformance

Publish a schema, canonicalisation vectors, signature vectors and malicious archive corpus. Test duplicate/case/Unicode paths, traversal, huge counts/sizes, compression bombs, invalid JSON/UTF-8/numbers, missing/extra files, signature substitution, stale catalogue, SemVer/range edges, component/world mismatch, helper target ambiguity, interrupted install/update and reproducible byte-for-byte builds.
