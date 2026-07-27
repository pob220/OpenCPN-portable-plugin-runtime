# Portable plugin security model

## Security objective and limits

The runtime reduces ambient authority and contains ordinary component failures. It does not make third-party navigation logic trustworthy, certify routes, validate every chart or forecast, or turn OpenCPN into a high-assurance hypervisor. Route, environmental and chart-safety outputs must carry provenance/uncertainty and a user-visible advisory status. `unknown` or missing coverage is never silently treated as safe.

WebAssembly memory isolation is one layer. Wasmtime and host imports are in the OpenCPN process, so runtime or host defects can still affect the application. Native helpers add a process boundary but remain native attack surface.

## Assets and actors

Assets: live navigation state; route/track/waypoint database; charts and licences; user files; environmental datasets; settings; provider tokens and private secrets; network identity; UI integrity; OpenCPN availability; catalogue/update trust; audit evidence.

Adversaries and failures include malicious plugin authors, compromised developer/CI signing identity, catalogue/mirror compromise, stale/replayed updates, vulnerable runtime/toolchain, spoofed provider plugin, malicious or malformed GRIB/chart/route input, compromised helper, and accidental runaway code.

## Trust boundaries

```text
untrusted package bytes
  -> bounded archive/manifest parser
  -> signature + catalogue + revocation verification
  -> compatibility/permission decision
  -> Wasmtime component (untrusted linear memory)
       -> validated WIT adapter
       -> policy-enforcing core services
       -> OpenCPN models/UI/OS
  -> optional helper supervisor
       -> authenticated bounded local protocol
       -> OS-sandboxed native process
```

Catalogue authorization and package integrity are separate checks. A valid signature identifies a signer; it is not a quality or navigation-safety endorsement. The user grants capabilities to the package identity and major version, not a filename.

## Default-deny permission vocabulary

Permissions are manifest-declared, user-visible and linked only after approval. Undeclared access is structurally unavailable.

| Permission | Scope/constraints | Default |
|---|---|---|
| `navigation.position.read` | selected vessel snapshot and bounded subscriptions | deny |
| `navigation.objects.read` | paged route/waypoint/track values | deny |
| `navigation.objects.write` | create/update/delete with confirmation and revision checks | deny; prominent |
| `charts.coverage` | coverage metadata only | deny |
| `charts.safety` | structured batched safety evidence | deny; advisory label |
| `environment.consume` | selected datasets/providers | deny |
| `environment.provide` | register typed provider; separate catalogue review initially | deny |
| `overlay.submit` | bounded retained scenes | deny |
| `ui.commands` / `ui.surfaces` | actions and host-owned role-aware surfaces | deny |
| `jobs.compute` | bounded worker allocation | deny |
| `settings` | namespaced small non-secret values | grant with install consent |
| `storage.private/cache/temp` | quota-bound plugin directories/resources | grant by class |
| `storage.user-open/save` | only files/locations explicitly selected by user | per operation or persisted grant |
| `network.https` | exact declared DNS domains, HTTPS, bounded methods/size/rate/redirects and public destinations | deny |
| `credentials.use:<provider>` | broker uses named credential; token injection preferred | deny; explicit |
| `network.raw` | unrestricted socket capability | absent in v1; strongest review if ever added |
| `helper.execute:<id>` | one signed declared helper/profile | deny; explicit experimental warning |

Permission expansion on update disables the plugin until consent. Domain
wildcards and IP literals are rejected; every redirect is revalidated,
sensitive headers are removed across origins, proxy inheritance is disabled,
and local/link-local/private/special destinations are rejected after DNS
resolution. Core may reduce grants below requested scope.

## Package and update trust

The package rules are detailed in [package-format.md](package-format.md). Security requirements:

- deterministic canonical manifest and SHA-256 digest for every file;
- detached signature binding package identity, version, manifest and content root;
- independent signed catalogue metadata containing expected digest/size/signer policy;
- threshold-managed offline root keys and separately delegated package publisher roles;
- monotonically versioned, expiring snapshot/timestamp/revocation metadata;
- rollback and freeze detection using last-known versions and trusted time bounds;
- atomic install into immutable version slots, then pointer switch; preserve previous slot/state for rollback;
- path canonicalisation, duplicate/case-collision rejection, no symlinks/devices, file/count/uncompressed-size/compression-ratio limits;
- keep package-installation limits distinct from environmental-data limits: an
  ocean-scale GRIB is an external capability-selected document, not a package
  entry, and is decoded message-by-message with bounded result batches;
- signature and revocation verification before component compilation or helper inspection/execution;
- an emergency kill entry for package identity/version and runtime/API profile.

This follows The Update Framework's separation of root, targets, snapshot and timestamp roles and its use of hashes, versions, expirations and signature thresholds ([TUF overview](https://theupdateframework.io/docs/overview/), [roles](https://theupdateframework.io/docs/metadata/)). The first prototype may use pinned test Ed25519 keys and offline fixtures, but production should adopt TUF-compatible metadata rather than inventing an updater.

Sigstore bundles can add publisher identity, CI provenance and offline-verifiable transparency material ([bundle format](https://docs.sigstore.dev/about/bundle/)). They do not replace OpenCPN's authorization/revocation policy; transparency also requires monitoring. Treat Sigstore as optional provenance attached to a TUF-authorised digest.

Developer installs use a distinct developer mode, visible warning and isolated trust root. They never become catalogue-trusted merely because a user accepted a one-off file.

## Runtime containment and quotas

Per instance/store:

- cap component/package size, compilation time, instances, memories/tables, linear-memory growth and host resources;
- use Wasmtime resource limiters plus fuel/epoch interruption;
- serialize entry to a component store; bound runnable tasks and host calls per plugin;
- require deadlines/cancellation for all long operations;
- rate-limit events, logs, UI updates, overlay commits, network requests and provider calls;
- bound list/string/geometry dimensions before allocation and use checked arithmetic;
- retract UI/actions/scenes/services immediately on trap or quarantine;
- drop the instance after a shutdown deadline; do not reuse its memory/resources;
- keep per-plugin failure counters and quarantine repeated startup/trap loops.

Provisional budgets are set by workload class and platform, not trusted manifest wishes. A user may lower but not exceed host policy. Fuel is not a precise portable CPU billing unit; measure wall/CPU time as well. A cooperative cancel is delivered first, epoch/fuel interruption second, instance drop last.

Host calls are more privileged than guest code. Each validates identity, permission, handle ownership, revision, size and cancellation, then copies/queues values before touching internal models. Never hold chart/nav locks while entering a component. Never call a plugin from destructors, signal handlers or exception unwinding.

Wasmtime guarantees only imported capabilities to Wasm and documents memory isolation, but explicitly tells embedders to validate guest values ([security](https://docs.wasmtime.dev/security.html)). Its supported branches receive security backports, while normal releases live only two months and LTS releases 24 months ([release policy](https://docs.wasmtime.dev/stability-release.html)). OpenCPN must track advisories, ship patched builds promptly and have a remote-disable plan.

## Files, networks, credentials and platform behavior

### Storage

Private/cache/temp roots are host-opened resources; the component need not see native paths. User files come from OpenCPN's native picker or portal and confer only the selected read/write capability. Reopen/persistence is an OS-specific bookmark/document grant controlled by the host. Resolve symlinks/races using safe open semantics and validate archive/data formats in bounded code or helper.

Flatpak uses the FileChooser/Documents portal and its application-private storage; it cannot delegate authority OpenCPN itself lacks ([FileChooser portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.FileChooser.html)). Windows and macOS use host file pickers and persisted access only where platform policy supports it. Raspberry Pi/native Linux use the same service semantics even when underlying file descriptors differ.

### HTTP

Prefer host HTTP over guest sockets. Enforce HTTPS by default, system trust, hostname validation, controlled redirects, proxy policy, declared domains/methods, maximum headers/body/decompressed bytes, streaming backpressure, cancellation, retry limits and provider-specific error codes. Block loopback, link-local, metadata-service and private-network targets unless separately authorised. Do not inherit any legacy downloader path that disables TLS verification.

### Secrets

Settings/cache never store credentials. A credential is keyed by plugin identity plus provider and held in OS facilities where available (Windows credential protection, macOS Keychain, Linux Secret Service/portal). Prefer the broker adding `Authorization` to an approved request, so the plugin receives no token bytes. If a provider protocol truly requires reveal, use a separate permission, short lifetime and redacted logs. Locked/unavailable secure storage fails closed; no plaintext fallback.

## Helper process security

Each helper entry declares exact target, executable digest, protocol version, required libraries/resources and capability profile. It is covered by the package signature and catalogue digest. The supervisor launches it from a read-only verified slot with a random authenticated pipe endpoint, minimal environment, private temporary directory, inherited handles only, no search-path library resolution and no shell.

Apply platform controls where practical: Windows Job Object/restricted token and process mitigations; macOS sandbox profile/hardened signing; Linux namespaces/seccomp/rlimits/cgroups; inside Flatpak, remain inside the existing sandbox. These require platform spikes and are not promised by the first slice.

The helper receives only job input/resources needed for the operation. Provider credentials should still be brokered by host HTTP. Protocol messages are length/framing/version checked and all helper output is hostile. Crash, bad message, timeout or memory excess fails the job, records diagnostics and terminates/restarts within a rate limit. No automatic download of missing helpers.

## Threat-to-control matrix

| Threat | Prevent/detect/respond |
|---|---|
| malicious plugin / host-service abuse | default-deny imports, typed validation, quotas, audit, disable/quarantine |
| compromised update or catalogue | independent signatures/digests, TUF roles/thresholds/expiry, transparency, rollback protection, emergency revocation |
| CPU/memory denial of service | fuel/epochs, limiters, worker quotas, deadlines, event/rate limits, helper OS limits |
| credential theft | scoped broker/injection, OS vault, no logs/settings, explicit consent/revocation |
| filesystem/network escape | no ambient WASI, resource handles, portals, HTTP allowlist; raw sockets absent |
| malformed GRIB/chart/route | limits and semantic validation, immutable chart service, native codec helper, fuzz corpora |
| plugin spoofing/inter-plugin confusion | catalogue-bound identity, supervisor-authenticated provider registration, typed interface/version |
| unsafe helper | signed target selection, process sandbox, authenticated protocol, least handles, kill/restart limits |
| runtime/compiler vulnerability | pinned supported release, SBOM/advisory monitoring, patch SLA, canary CI, remote tier disable |
| UI deception/flood | host-rendered controls, plugin identity label, no trusted chrome, update quotas and focus/accessibility rules |
| false navigation confidence | structured unknown/conflict/quality/provenance, advisory labels, no “safe” claim beyond stated evidence |

## Audit and privacy

Record package identity/version/digest, trust decision, permission changes, lifecycle failures, quota terminations, helper launches/exits, credential use (not value), network destination class and core mutations. Use bounded rotating logs, stable diagnostic ids and redaction. Do not log precise position/routes by default; detailed debug capture requires opt-in and warns about sensitive navigation data.

## Security release responsibility

Production requires named owners for runtime advisories, package/catalogue roots, API security review and disclosure response. Critical actions: publish patched OpenCPN/runtime builds; revoke malicious packages/keys; expire compromised metadata; communicate affected versions; preserve audit evidence; provide a local switch disabling portable execution. If nobody accepts this rota, the feature must remain experimental and disabled by default.
