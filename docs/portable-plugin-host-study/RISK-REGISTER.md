# Risk register

Likelihood/impact are current pre-migration assessments: low, medium or high.

| ID | Category | Risk | L | I | Mitigation / release gate |
|---|---|---|:---:|:---:|---|
| R1 | Technical | A panic/UB/allocator failure in the native host or linked Wasmtime aborts OpenCPN | M | H | Project-signed preview only; fuzz/sanitise bridge; narrow C ABI; external broker before open third-party production |
| R2 | Technical | Long guest or host callback blocks the UI thread | M | H | Callback enqueue only; per-generation serial executor; fuel/epoch/deadlines; measured UI latency gate |
| R3 | Technical | Detached worker/helper survives plugin unload | M | H | Fixed shutdown barrier, cancellation tokens, bounded join, process-tree kill; repeat enable/disable/shutdown tests |
| R4 | Technical | GL callback leaks state or retained scene stalls rendering | M | H | Immutable bounded snapshots, scoped GL state, no guest/IPC in render callback, software/GL frame budgets |
| R5 | Toolbar lifecycle | Late insertion workaround changes or rebuild loses state | M | M | Generic stable-action API proposal; automated stock-version integration tests; one-icon/menu fallback |
| R6 | Toolbar lifecycle | Transient native ID is persisted or routed to wrong generation | L | H | Only `(package_id, action_id)` persists; bidirectional collision-checked map; generation token on every queued click |
| R7 | Toolbar UX | No native disabled state causes a clickable-looking initialising tool | H | M | Hide until ready or substitute explicit busy artwork and block dispatch; require generic enabled state for beta |
| R8 | Toolbar UX | Core customisation cannot place/hide child tools durably | H | M | Host preferences and honest release note; stable-action persistence proposal |
| R9 | Security | Malformed SVG/raster exhausts decoder or exploits image stack | M | H | Canonical in-package path, encoded/decoded quotas, dimensions/format allowlist, sanitisation/rasterisation and negative corpus |
| R10 | Security | Archive traversal, collision or decompression bomb writes outside package root | L | H | Retain current strict validator, entry/size limits, no links, atomic staging; 18 negative package tests are mandatory |
| R11 | Security | Development signing key is mistaken for a production trust root | M | H | Build-time separation, visible developer-mode marking, production root ceremony/rotation/revocation before beta |
| R12 | Security | Child update channel becomes an ungoverned executable ecosystem | H | H | Named catalogue owner, signer policy, review criteria, emergency revocation and support SLA; otherwise project-signed packages only |
| R13 | Security | Permission update silently expands access | M | H | Store manifest hash and grants, calculate delta, explicit re-consent, fail closed on unknown/removed policy |
| R14 | Security | Credential leaks through config/log/arguments/helper environment | M | H | Session memory initially, redaction tests, scoped pipe/OS store; no persistent plaintext; generic keychain proposal |
| R15 | Security | Native helper escapes expected containment | M | H | Treat helper permission as native code; signed target payload, minimal mounts/environment, OS limits, broker supervision; publish unequal guarantees |
| R16 | Data/safety | GSHHS/display mask is represented as chart/depth safety | H | H | Rename service, explicit advisory/unknown states, no routing safety claim; generic chart API only after semantic review |
| R17 | Data | Route/waypoint mutation applies stale or oversized guest data | M | H | Revision/timestamp, quotas, user preview/confirmation, value validation and atomic/rollback semantics |
| R18 | Maintenance | Wasmtime vulnerability/update cadence exceeds plugin releases | M | H | SBOM, subscribed advisories, reproducible target builds, emergency host release path; broker/runtime version visible |
| R19 | Maintenance | WIT/package/action versions drift incompatibly | M | M | Independent semantic versions, declared compatible ranges, golden older packages and fail-closed required features |
| R20 | Maintenance | Extracted environment/routing monoliths preserve hidden core globals | H | M | Adapter-only `ocpn_plugin.h`; dependency tests/forbidden include checks; module decomposition before copy |
| R21 | Distribution | Wasmtime/native libraries collide with OpenCPN or another plugin | M | H | Hidden symbols/private shared library or broker, loader/rpath tests, never discover another plugin's runtime |
| R22 | Distribution | Managed installer-owned directory is mutated by child updates | M | H | Read-only host resources; child store below OpenCPN private data; install/update negative test |
| R23 | Distribution | 22+ MiB runtime and helpers exceed practical package/update expectations | M | M | Measure per target, feature/link optimisation, delta/cache policy; never trade away limits solely for size |
| R24 | Platform | Flatpak blocks helper spawning/network/files or nested sandboxing | H | H | Run actual SDK conformance, use portals/scoped files, assume no privilege expansion, offer local-file/reduced mode |
| R25 | Platform | macOS hardened runtime rejects JIT/helper or notarisation | M | H | Target-specific entitlement/signing/notarisation proof before advertising; broker/AOT/interpreter evaluation |
| R26 | Platform | Windows DLL search or process cleanup is unsafe | M | H | Private absolute paths, safe loader flags, Job Object kill-on-close, signed installer artifacts and crash tests |
| R27 | Platform | Android is inferred from desktop portability claims | H | M | Explicitly deferred/unsupported in manifests/docs; dedicated arm64/AOT/interpreter study later |
| R28 | UX | Toolbar children are mistaken for first-class managed plugins | H | M | Call them portable packages/child extensions; manager UI explains outer ownership and separate update/permission channel |
| R29 | UX | One host failure removes all child functionality | M | H | Per-generation health and retraction, broker recovery, safe host manager remains available, attributed diagnostics |
| R30 | Governance | No party owns trust roots, review, revocation and incident response | H | H | Hard production gate: written owner/RACI, response SLA and catalogue policy; otherwise no third-party execution |
| R31 | Upstream | Generic action API is rejected or delayed | M | M | Keep proven public workaround and menu fallback; submit independently with ordinary plugin use cases/tests |
| R32 | Upstream | First-class child identity is not accepted | H | M | Preserve host-managed identity; do not make native manager entries a beta prerequisite |
| R33 | Testing | Linux proof is overgeneralised to all desktop targets | H | H | Per-target executed conformance; omit unsupported metadata targets; distinguish architecture from validation |
| R34 | Testing | Concurrent iWeatherRouting work contaminates study/migration commits | M | H | Build from pinned archives, path-limited staging, never stash/discard shared edits, verify staged diff before every commit |

## Stop/ship rules

- Any failure of signature/path/target/permission checks is fail-closed.
- No public third-party package catalogue ships while R11/R12/R30 lack named
  owners and tested revocation.
- No target is labelled supported while R24–R27/R33 lack executed evidence.
- No authoritative safety language ships while R16 remains unresolved.
- Repeated unbounded unload, stale callback or host crash failures block beta;
  they are not documentation-only limitations.
