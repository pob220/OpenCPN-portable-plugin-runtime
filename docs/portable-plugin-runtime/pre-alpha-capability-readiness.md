# Pre-alpha portable capability readiness

## Implemented foundation

- The installed `0.1` component world remains loadable.
- A modular `0.2` lifecycle/event/messaging contract is compiled during
  bridge tests but is not yet selected by production packages.
- Manifest-declared event subscriptions use exact event kinds, bounded topic
  prefixes and per-package queue limits.
- High-rate state events coalesce by topic. Ordered streams such as NMEA 0183
  and plugin messages drop the oldest entry at their declared bound.
- Plugin-message receive and send paths have separate permissions. Guest
  output is marshalled through the OpenCPN UI service boundary.
- The native-to-portable `0.1` compatibility adapter uses the existing
  `on-surface-event` export with the reserved `host.plugin-message` identity.
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
| Compatibility | iGRIB, iPolars and iWeatherRouting compile and pass their existing lifecycle tests |

## Required before an OpenCPN alpha publication

1. Instantiate `portable_api 0.1` and `0.2` side by side, selecting the world
   from the signed manifest rather than changing an interface in place.
2. Move the `0.1` plugin-message compatibility adapter to the typed `0.2`
   message sink and add subscription/unsubscription imports.
3. Connect the broker to NMEA 2000, Signal K, AIS, active-leg, cursor and
   viewport producers, retaining the same queue and permission policy.
4. Add typed navigation-object read/write capabilities with explicit
   user-confirmation policy for mutations.
5. Add canvas/input, notifications/timers/sound and richer declarative UI
   probes.
6. Extend the independently tested host-owned CM93 chart-safety provider with
   the corresponding stock-compatible S-57 backend and cross-platform chart
   fixtures.
7. Run the signed package/conformance suite on Linux x86_64/aarch64, Flatpak,
   Windows and macOS before updating the alpha catalogue.

The current slice is suitable for continued Linux development and for
building the cross-platform test matrix. It is not yet a sufficient basis for
publishing general third-party portable plugins as OpenCPN alpha packages.
