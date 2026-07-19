# Portable runtime prototype status

Status date: 2026-07-19. Baseline: OpenCPN Release_5.14.0 (`91f3b...`) in the
separate Test-OpenCPN build/profile. This file records implementation evidence;
it does not upgrade any experimental interface into a compatibility promise.

## Implemented vertical slice

- Build flag `OCPN_ENABLE_PORTABLE_PLUGINS`, default `OFF`, and independent
  runtime option `/PortablePlugins/EnableExperimental`, default false.
- Parallel portable loader; the native loader and native plugin ABI are intact.
- Wasmtime 46.0.1 Component Model bridge with WIT-generated bindings, inert
  WASI defaults, 256 MiB store limit, finite fuel and an engine epoch ticker.
- Bounded manifest discovery, exact prototype runtime/API negotiation,
  permission allow-listing and exported component/manifest identity matching.
- Typed value services for command registration, vessel position, namespaced
  settings, retained geographic polyline scenes, cancellable jobs and the
  transitional environmental-viewer adapter.
- Component traps are converted to plugin failure; its scenes/jobs are removed
  or cancelled and OpenCPN remains operational.
- Deterministic `.ocpnp` producer and a developer installer which rejects path
  traversal, Unicode/case collisions, cross-platform reserved names, links and
  non-regular files, zip bombs, oversized archives, duplicate JSON keys,
  undeclared files, digest mismatch and non-Wasm components.
- Installable Rust Component Model iGRIB proof package. In compatibility mode
  it opens the existing xGRIB UI through one typed host operation; no native
  object crosses the component boundary.

## Verified on this host

- Full Test-OpenCPN 5.14.0 application builds and links with `-Werror`.
- CTest discovers and passes the real-component lifecycle/fault/identity test
  and ten malicious/reproducible-package tests.
- A clean feature-disabled build reaches 100%; its OpenCPN link command and
  symbol table contain no portable-runtime manager, bridge or Wasmtime entry.
- The final `.ocpnp` archive is reproducible across independent invocations
  (`ad8aa12477d2bace995e70caa859eccc58f336f1c8a28ea90502ac66567b6174`
  on this build) and passes a fresh developer install.
- Isolated GUI launch loads iGRIB, invokes its action, persists its setting,
  submits and renders a retained overlay, completes a background job and opens
  the installed xGRIB window with the current GRIB dataset.
- A deliberate guest trap is contained, followed by normal OpenCPN operation
  and clean process exit.
- The stock non-REST test set passes 56/56 and buffer tests pass 8/8. The five
  REST cases cannot be isolated on this host because their fixed endpoint
  reaches the intentionally untouched, already-running 5.15.0 instance; that
  external version collision is recorded rather than stopping the working app.
- The concurrently running modified OpenCPN 5.15.0 process/profile was not
  stopped, written or used for the tests.

## Not implemented / not production-ready

- Production signing, catalogue/TUF metadata, revocation, consent UI, updates,
  rollback and state migration.
- A general host-rendered declarative UI, HTTP client, credential broker,
  capability storage, environmental dataset service, chart-safety batching,
  route services, provider registry or native-helper supervisor.
- Independent iGRIB environmental decode/generation. The exact xGRIB UI in this
  proof depends on an enabled native xGRIB provider and is a migration adapter,
  not the eventual platform-neutral implementation.
- Guest execution is synchronously entered by the prototype UI dispatch path;
  fuel/epoch limits bound failure, but production work requires a supervised
  runtime executor so long calls never occupy the GUI thread.
- ARM64, macOS, Windows and Flatpak builds/measurements. Portability outside the
  tested Linux x86-64 host remains an architectural/toolchain conclusion, not
  measured evidence.

## Next gates

1. Review the RFC and this slice before broadening the WIT world.
2. Move component entry calls to a serial supervised executor and marshal host
   services onto their owning executors.
3. Implement host HTTP, private storage/user-selected files and a minimal
   declarative progress/form surface.
4. Add immutable environmental dataset handles plus batch sampling; prototype
   the ecCodes/NetCDF/PROJ helper protocol out of process.
5. Add chart segment batching and the routing reference component, then run the
   benchmark and cross-platform conformance plan.
6. Select and implement production package/catalogue trust only after ownership
   and security-response governance are accepted.
