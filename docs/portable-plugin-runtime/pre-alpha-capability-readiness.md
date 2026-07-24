# Pre-alpha portable capability readiness

Status date: 2026-07-24.

## Implemented foundation

- The Portable Plugin Manager is a conventional OpenCPN plugin. It supplies
  the portable runtime to an otherwise stock OpenCPN build; no portable loader
  changes are required in OpenCPN core.
- Installed API `0.1` and `0.2` components remain loadable. API `0.3` adds the
  general third-party author profile without changing the bundled
  applications' signed API selection.
- API `0.3` has typed, permission-gated interfaces for:
  - toolbar and chart-context actions, including runtime state and removal;
  - host-owned declarative surfaces and scoped user-file grants;
  - private atomic storage and simple persistent settings;
  - vessel position and bounded waypoint/route/track reads;
  - user-confirmed waypoint/route/track mutation;
  - retained renderer-independent scene layers containing polylines,
    polygons, circles, package icons and text;
  - pointer/key input, with package-scoped interactive scene hits;
  - NMEA 0183 output;
  - one-shot/repeating timers;
  - native OpenCPN plugin-message compatibility;
  - asynchronous typed package-to-package RPC.
- The bounded event broker supplies NMEA 0183, common NMEA 2000 PGNs,
  Signal K, vessel position, AIS, active-leg, cursor, viewport and native
  plugin-message events. Signed manifest subscriptions and dynamic API `0.3`
  subscriptions use the same permissions, topic filtering, queue limits and
  lifecycle revocation.
- High-rate state events coalesce by topic. Ordered streams drop the oldest
  entry at their declared bound. All guest calls run on package-serial
  executors with generation checks.
- Scene updates have monotonic revisions and aggregate limits of 128 layers,
  4,096 primitives and 250,000 line/polygon points. Rendering is host-owned
  and uses the same scene data in software/Vulkan and OpenGL presentation
  paths.
- Timers and RPC are lifecycle-owned. Disable, failure and unload revoke
  timers, registrations and outstanding calls; RPC dispatch is asynchronous
  and cannot synchronously re-enter another Wasm store.
- Navigation mutation is value-only and always crosses an explicit OpenCPN
  confirmation boundary.
- A standalone Rust author kit creates a pinned project, lints its manifest,
  builds it and creates deterministic development packages.
- A headless fake host runs the API `0.3` template through the production
  bridge and exercises lifecycle, actions, settings, declarative surfaces,
  scenes, timers, storage and RPC registration.

## Capability probes

| Probe | Current assertion |
|---|---|
| Compatibility | API `0.1`, modular API `0.2`, and author API `0.3` components use the same bridge |
| Event broker | Prefix filtering, FIFO sequence, queue bound, drop count, state coalescing and payload bound |
| Live producers | NMEA 0183/2000, Signal K, position, AIS, active leg, cursor and viewport reach only permitted subscriptions |
| Actions | Toolbar/context registration, state changes and lifecycle removal are host-owned |
| Navigation | Reads are bounded; writes require user confirmation; NMEA output is validated and rate-limited |
| Scenes/input | Retained primitives render through host paths and interactive hits are delivered only to the owning package |
| Storage | Private writes are atomic; user files require short-lived host grants |
| Timers | Package quotas, minimum/maximum periods and lifecycle cancellation are enforced |
| RPC | Provider ownership, correlation IDs, timeouts, payload limits and lifecycle cleanup are enforced |
| Author tooling | Scaffold, lint, deterministic package and fake-host conformance tests pass |

## Third-party source audit

The detailed audit is in
`docs/portable-plugin-runtime/third-party-portability-audit.md`. In summary:

- `testplugin_pi` can be rewritten against API `0.3`; its ODAPI-style JSON
  exchange should become typed asynchronous RPC.
- `windvane_pi` has the required event, overlay, toolbar, persistence and NMEA
  output building blocks.
- `ocpn_draw_pi` has the necessary portable architectural primitives, but it
  is a substantial rewrite. Its wxWidgets dialogs and immediate-mode
  OpenGL/wxDC drawing must become host-owned declarative surfaces and retained
  scenes. Exact object hit-testing remains guest geometry driven; the host hit
  hint is deliberately only a bounded coarse filter.
- `crowdsource_pi` is **not yet portable-ready**. Its raw socket transport is
  intentionally unavailable, and the current host network capability is
  download-only. It requires a constrained asynchronous HTTPS request/upload
  capability and a compatible server endpoint.

## Required before an OpenCPN alpha publication

1. Specify and implement a bounded asynchronous HTTPS request/upload service:
   TLS-only by default, explicit method/header policy, request and response
   quotas, redirect policy, cancellation, credential-provider integration and
   no raw sockets.
2. Add API `0.3` integration fixtures through the real Manager for action,
   scene/input, surface/file, timer and two-package RPC lifecycle behavior;
   the fake host remains the fast author test.
3. Complete accessibility, DPI, multi-canvas and failure-path GUI testing for
   declarative surfaces and retained scenes.
4. Extend the independently tested host-owned CM93 chart-safety provider with
   the corresponding stock-compatible S-57 backend and cross-platform chart
   fixtures.
5. Run signed package and conformance suites on Linux x86_64/aarch64, Flatpak,
   Windows and macOS before updating the alpha catalogue.

The current tree is a strong Linux development proof and a usable authoring
baseline for non-network-server plugins. It is not yet an honest claim that
every native OpenCPN plugin can be mechanically ported or that general
third-party packages are ready for the OpenCPN Alpha catalogue.
