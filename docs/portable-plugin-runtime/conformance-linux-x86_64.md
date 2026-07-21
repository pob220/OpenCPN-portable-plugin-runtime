# iGRIB conformance report: Linux x86-64

Date: 2026-07-20

Host: Linux 6.18.37-1-lts, x86-64, glibc 2.43. OpenCPN: isolated
Test-OpenCPN 5.14.0 experimental build. Target identifier:
`linux-gnu-x86_64`.

## Automated package/helper run

Command:

```text
python3 portable-runtime/tests/conformance.py \
  --package <build>/org.opencpn.igrib-0.1.0.ocpnp \
  --trusted-key portable-runtime/development-keys/igrib-ed25519-public.pem \
  --fixture <44.8-MB-environment-fixture.grb> --full-generator
```

| Check | Result | Observation |
|---|---:|---|
| signed package verification/fresh install | pass | development Ed25519 key; complete digest inventory |
| exact target helper selection | pass | `linux-gnu-x86_64` |
| malformed input containment | pass | 3.550 ms; structured failure |
| generator protocol negotiation | pass | schema 1; 6.671 ms |
| real fixture inspection | pass | 252.555 ms; 44,865,184 bytes; 465 messages; 84 times |
| first/last frame decode | pass | 479.067/402.569 ms; two distinct frames; 7,760 bounded samples per frame |
| real generator output | pass | 406.233 ms; validated 44,865,184-byte output using existing-file provider |

These timings are one conformance observation, not statistically useful
benchmarks. Run the repetition/percentile plan in `performance-plan.md` before
choosing service budgets.

## Live Test-OpenCPN run

- iGRIB loaded and opened while native xGRIB was absent from the plugin search
  path.
- The accessible UI exposed file/timeline/playback, five environmental layer
  toggles, settings, download, generation, progress and cancellation.
- The 44.8 MB fixture opened through the native picker and rendered retained
  frames. At the hourly 19:00 step, iGRIB retained 62,910 samples and reported
  that the product's three-hourly wave height, peak period and direction fields
  came from 18:00. Playback advanced to the next forecast time.
- Existing display preferences migrated to current arrows without scalar
  circles by default; the optional magnitude overlay remains user-selectable.
- Wave rendering exposed crest-and-travel, travel-arrow and
  height-circle/direction forms with configurable symbol colour, size and
  spacing. The native picker remembered the last successfully loaded GRIB
  directory in only the isolated iGRIB profile.
- One batched request evaluated four chart-coverage segments.
- The host HTTP test fetched 7,655 bytes and read them through plugin-private
  storage.
- A live minimal GFS job downloaded forecast data under bubblewrap containment,
  published `/tmp/igrib-gui-final.grb` (1,246 bytes, six GRIB messages, two
  forecast times), reopened it and retained 75 samples for display.
- A component trap and a malformed decoder input were contained. OpenCPN
  remained responsive and subsequently completed normal operations.

## iWeatherRouting interoperability run

- The signed iWeatherRouting package and its complete declarative control
  schema passed package conformance. The exact final Test-OpenCPN executable
  loaded both components in an isolated `/tmp` profile, discovered iGRIB's
  `org.opencpn.environment.provider` service and opened the host-rendered
  iWeatherRouting window with its Calculate and Cancel controls.
- The runtime smoke test exercised typed environmental batches and chart
  batches, calculated routes in four concurrently isolated Wasmtime Stores,
  selected a result, contained cancellation and shut every replica down. A
  replica does not rerun plugin lifecycle registration.
- Multi-step route conformance also verified bounded isochrone and predecessor
  trace transfer. The host's variable-departure table retains failed and
  successful candidates and makes one selected candidate the sole source for
  the emphasized route, isochrones, route-to-cursor inspection, forecast-time
  boat marker and GPX export.
- Routing now preflights every comparison departure against the selected iGRIB
  dataset. The bounded frame cache consumes each requested time before later
  decodes can evict it, and the bridge smoke test verifies that a provider's
  structured diagnostic crosses host ABI 9 to the routing result.
- The host polar parser loaded the bundled 15-by-16 Nicholson 35 Mk1 `.pol`,
  resolved it through an OpenCPN boat `.xml`, rejected malformed axes and sent
  only bounded typed grids to the component. The routing smoke test verified
  that reducing the supplied grid speeds produces a later arrival, proving the
  guest calculation uses the selected polar values.
- This is proof of the portable inter-plugin/service path, not a navigation
  validation. The reference component uses a bundled conservative polar and
  GSHHS shoreline screening; commissioned polar validation, XML crossover
  contours and structured hydrographic safety evidence remain explicit later
  gates.

## Platform status

Only Linux x86-64 passed in this report. Windows x86-64, macOS x86-64,
macOS ARM64, Linux ARM64/Raspberry Pi, Flatpak x86-64 and Flatpak ARM64 are
`untested`, not inferred passes. Each target must run the same script with its
target helper payload and complete the GUI/process-sandbox checklist before it
can be marked supported.
