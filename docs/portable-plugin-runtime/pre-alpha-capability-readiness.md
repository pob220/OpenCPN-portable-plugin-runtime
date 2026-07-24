# Pre-alpha portable capability readiness

## Implemented foundation

- The installed `0.1` component world remains loadable and has a dedicated
  compatibility fixture.
- Signed manifests select side-by-side `0.1`, modular `0.2`, or the extended
  `0.2` weather-routing world.
- iGRIB, iPolars, iWeatherRouting and capability-lab use `0.2`.
- Host capabilities are separated into diagnostics, actions, navigation,
  settings, scenes, jobs, surfaces, environment, routing control,
  chart safety, network, storage and plugin-message interfaces.
- Guest exports are separated into lifecycle, surface, job, event,
  plugin-message and optional weather-routing sinks.
- Host and guest service failures use structured error records rather than
  untyped strings.
- Manifest-declared event subscriptions use exact event kinds, bounded topic
  prefixes and per-package queue limits.
- High-rate state events coalesce by topic. Ordered streams such as NMEA 0183
  and plugin messages drop the oldest entry at their declared bound.
- Plugin-message receive and send paths have separate permissions. Guest
  output is marshalled through the OpenCPN UI service boundary.
- The native-to-portable `0.1` compatibility adapter uses the existing
  `on-surface-event` export with the reserved `host.plugin-message` identity;
  `0.2` uses its typed message sink.
- A development-only capability-lab component proves filtered NMEA input and
  bidirectional plugin messaging without adding test behavior to iGRIB,
  iPolars or iWeatherRouting.

## Capability probes

| Probe | Current assertion |
|---|---|
| Event broker | Prefix filtering, FIFO sequence, queue bound, drop count, state coalescing, payload bound |
| Runtime event path | Matching NMEA is delivered; non-matching NMEA is not |
| Native → portable messaging | Only a declared message prefix reaches the guest |
| Portable → native messaging | Permission-gated host callback receives the bounded message |
| Manifest policy | A subscription without its corresponding permission fails static load |
| Compatibility | A legacy `0.1` guest and all four migrated `0.2` guests run through the same bridge |
| World selection | Base and weather-routing worlds are selected from signed manifest fields |
| Structured errors | Host failures cross the `0.2` boundary as code/message/retryable records |

## Required before an OpenCPN alpha publication

1. Finalise ownership and revocation semantics before importing the reserved
   dynamic subscription/unsubscription interface; signed manifest
   subscriptions remain the current authority.
2. Connect the broker to NMEA 2000, Signal K, AIS, active-leg, cursor and
   viewport producers, retaining the same queue and permission policy.
3. Add typed navigation-object read/write capabilities with explicit
   user-confirmation policy for mutations.
4. Add canvas/input, notifications/timers/sound and richer declarative UI
   probes.
5. Extend the independently tested host-owned CM93 chart-safety provider with
   the corresponding stock-compatible S-57 backend and cross-platform chart
   fixtures.
6. Run the signed package/conformance suite on Linux x86_64/aarch64, Flatpak,
   Windows and macOS before updating the alpha catalogue.

The current slice is suitable for continued Linux development and for
building the cross-platform test matrix. It is not yet a sufficient basis for
publishing general third-party portable plugins as OpenCPN alpha packages.
