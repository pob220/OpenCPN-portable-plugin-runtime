# Portable runtime implementation status

Status date: 2026-07-19. Baseline: OpenCPN Release 5.14.0 in the separate
Test-OpenCPN build/profile. The native xGRIB library was disabled and then
moved to Test-OpenCPN's recoverable `plugins-disabled` directory for the final
standalone test. The modified working OpenCPN 5.15 setup was not used.

This is implementation evidence, not a production compatibility or navigation
safety claim. Both the build option and runtime preference remain off by
default.

## Implemented

- Parallel Component Model loader using Wasmtime 46.0.1; the native loader and
  C++ ABI remain intact.
- Bounded manifest discovery, runtime/API negotiation, permission allow-list,
  component/manifest identity matching, inert ambient WASI, 256 MiB component
  memory limit, finite fuel and epoch interruption.
- Typed WIT services for actions, vessel-position values, namespaced settings,
  retained geographic overlays, cancellable jobs, environmental viewer,
  chart-coverage batches, host HTTP-to-private-storage and bounded private
  reads. No pointers, wxWidgets objects or graphics handles cross the boundary.
- Host-rendered declarative iGRIB surface with timeline navigation/playback,
  toggles for wind/current/pressure/waves/temperature, progress/cancellation,
  open/settings/download/generate actions and native file pickers.
- Standalone ecCodes helper supporting bounded `inspect` and `frame` operations
  with structured result/error JSON and atomic result publication.
- Environmental generator helper using versioned job/result/progress messages,
  GFS and UKMO providers, optional waves and TPXO current inputs, user-selected
  output and automatic reopening of completed output.
- Linux helper containment with bubblewrap namespaces and explicit immutable
  input/output/CA grants, `prlimit` CPU/address-space limits, child termination
  on cancellation/disable and deterministic cleanup. Other OS supervision
  policies are designed but not yet implemented or tested.
- Deterministic `.ocpnp` packages, complete SHA-256 file inventory, Ed25519
  signature verification, executable-path restrictions, malicious archive
  rejection, immutable extracted files, atomic replacement and retained
  rollback packages. The checked-in key is development-only.
- Reusable cross-platform conformance runner which reports pass/skip rather
  than extrapolating support.

## Linux x86-64 evidence

- Full Test-OpenCPN links with `-Werror`.
- Three CTests pass: real component lifecycle/trap/identity and typed service
  exercise; 15 package/signing/update security tests; target package/helper
  conformance.
- Signed package install and replacement preserve executable helper modes and
  retain the previous version in `.rollback`.
- A 44,821,471-byte real GRIB fixture was decoded as 387 messages, seven
  supported environmental field groups and 67 forecast times. A frame retained
  5,960 samples. Timeline playback advanced and overlays remained responsive.
- Malformed input returned a structured `environment-decode-failed` result and
  did not affect OpenCPN.
- The generator helper copied/validated the 44.8 MB fixture under containment
  and published a valid 387-message GRIB.
- The actual GUI generated a one-degree, one-hour NOAA GFS product over HTTPS,
  wrote a 1,246-byte/six-message GRIB, reopened it and displayed 75 retained
  samples. An initial missing CA mount and wxJSON string-conversion bug were
  detected by this test and fixed before the final run.
- The component's host HTTP action downloaded 7,655 bytes and read them back
  through namespaced private storage.
- A four-segment chart coverage request completed as one batch. A deliberate
  Wasm trap was contained and OpenCPN continued operating.
- Final GUI testing ran with native xGRIB absent from the Test plugin search
  path. iGRIB therefore has no enabled or loadable native xGRIB provider.

Measured conformance values for this machine are recorded in
`conformance-linux-x86_64.md`. Values are observations, not release budgets.

## Still experimental or incomplete

- Windows x86-64, macOS Intel/Apple Silicon, Linux ARM64/Raspberry Pi and
  Flatpak have not been built or executed in this session. Their helper
  binaries, signing/notarisation and OS-specific process controls remain gates.
- The package assembled here contains the Linux x86-64 helper payload. A single
  multi-target archive requires the central build service to add all signed
  target helpers; the Wasm component itself is unchanged across targets.
- The UI is an xGRIB-style functional surface, not a pixel-for-pixel port of
  every xGRIB preference, cursor interpolation, contour option or provider
  dialog. Broader parity should be incremental, not an expansion of the WIT
  boundary into wxWidgets.
- Chart coverage batching is real, but structured land/depth/drying/conflict
  safety evidence and route-shaped immutable caches are not yet implemented.
- Production catalogue/TUF metadata, revocation, user permission-consent UI,
  OS credential stores, state migrations and a security-response ownership
  agreement remain absent.
- Guest entry dispatch and some host callbacks still need a dedicated serial
  runtime executor before production so no potentially long guest call can
  occupy the wx event thread.
- CPU and memory quotas are enforced for the component and Linux helpers, but
  per-plugin fair scheduling, cgroup/job-object/App Sandbox profiles and robust
  helper restart throttling remain production work.

## Next release gates

1. Run the same conformance package on every target and publish target-built
   helpers in one signed multi-target archive.
2. Add Windows Job Object/restricted-token, macOS sandbox/hardened-runtime and
   Flatpak policy tests; add Linux seccomp/cgroup controls where deployable.
3. Move runtime entry to a serial supervisor executor and add shutdown/leak
   soak tests.
4. Add immutable environment dataset handles plus sample batches, then the
   structured chart-safety service and routing reference component.
5. Complete catalogue trust/revocation/consent and select an owned Wasmtime LTS
   before any production enablement.
