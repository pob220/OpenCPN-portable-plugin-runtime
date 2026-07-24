# Portable runtime implementation status

Status date: 2026-07-24. The current deployment baseline is the separate
RuntimeHost OpenCPN profile using stock OpenCPN Release 5.14.0 and the
conventional Portable Plugin Manager. The earlier Test-OpenCPN core-integrated
prototype remains historical evidence; the modified working OpenCPN 5.15
setup is not required by the current loader architecture.

This is implementation evidence, not a production compatibility or navigation
safety claim. Portable support remains opt-in by installing/enabling the
Manager and approving individual package capabilities.

## Implemented

- Conventional native Portable Plugin Manager using Wasmtime 46.0.1; stock
  OpenCPN's loader and C++ ABI remain intact.
- Bounded manifest discovery, runtime/API negotiation, permission allow-list,
  component/manifest identity matching, inert ambient WASI, 256 MiB component
  memory limit, finite fuel and epoch interruption.
- Typed WIT services for actions, vessel-position values, namespaced settings,
  retained geographic overlays, cancellable jobs, environmental viewer,
  time-indexed environmental sample batches, cancellable weather-routing,
  chart-coverage batches, host HTTP-to-private-storage and bounded private
  reads. No pointers, wxWidgets objects or graphics handles cross the boundary.
- Manifest-declared, versioned service discovery connects iGRIB's
  `org.opencpn.environment.provider@0.1.0` implementation to iWeatherRouting's
  compatible SemVer range without native-plugin broadcast strings. Invalid
  versions/ranges are rejected and missing/incompatible providers fail closed.
- iWeatherRouting's adaptive time-layer route search executes in the portable
  Rust component. The host supplies a schema-validated declarative UI whose
  tabs and required form controls are package metadata, typed
  iGRIB samples, batched host-owned chart checks, cancellation, progress, retained route
  alternatives and user-selected GPX output. A bounded departure window uses
  up to four independent Wasmtime Stores concurrently and selects the earliest
  safe arrival. Route endpoints can be resolved from the live vessel position,
  stable value snapshots of OpenCPN waypoints, the latest chart cursor or
  manual coordinates; no core navigation object crosses the boundary.
- The standard Portable Plugin Manager now owns a renderer-independent
  `charts.segment-safety` capability. Its stock-5.14-compatible CM93 decoder
  reads configured chart roots without an OpenCPN core patch, caches immutable
  cells and classifies `LNDARE`, `DRGARE`, `ITDARE` and `DEPARE/DRVAL1`.
  Semantic land/drying checks run on routing workers during propagation using
  exact CM93 cell-grid traversal; only unresolved advisory GSHHS fallbacks are
  marshalled to OpenCPN's main thread. Every delivered route then undergoes
  15-minute/~1.5 NM five-line corridor validation with the configured minimum
  depth. Strict requests fail closed on missing semantic/depth evidence. The
  public GSHHS check remains an explicitly advisory fallback only.
- Host-rendered declarative UI v2 environmental surface with readable UTC timeline
  navigation/playback, chart-cursor values, progress/cancellation,
  open/settings/download/generate actions and native file pickers. Persistent
  per-field display profiles cover units, vectors, colour overlays, numeric
  labels, spacing and relevant contours. Wind supports proper meteorological
  barbs or direction arrows; currents support single, double or filled-head,
  tidal-stream-style magnitude-proportional arrows. The proportional baseline
  size and pixels-per-knot growth are independently adjustable from vector
  spacing. Currents default to arrows without the scalar spot layer, while the
  magnitude overlay remains an explicit setting. Every field has an
  independently selectable display colour. Waves have independent symbol
  spacing and size plus crest-and-travel, travel-arrow and
  height-circle/direction forms; the selected colour applies to the complete
  symbol and optional scalar-map outline as well as values. The host filters
  waves and currents through a cached point-in-polygon index built from the
  best bundled shapefile basemap; wind, pressure and air temperature remain
  visible over land. The same marine-only policy applies to typed environmental
  batches consumed by iWeatherRouting. Both the decoder and host reject GRIB
  missing-value sentinels and current vectors at or above 12 m/s before they
  reach drawing, cursor readout or route sampling.
- The package owns all thirteen xGRIB field groups, surface/850/700/500/300 hPa
  levels, provider metadata and controller state. The generic host renders
  vector, scalar, number and contour forms for wind, gust, pressure, waves,
  current, precipitation, cloud, air/sea temperature, CAPE, reflectivity,
  geopotential height and relative humidity; it contains no provider IDs or
  iGRIB-specific dialog class. Multi-file datasets are immutable ordered
  snapshots with deterministic last-message-wins duplicate handling.
- Standalone ecCodes helper supporting bounded `inspect`, `frame`, interpolated
  frames and cursor weather-table operations
  with structured result/error JSON and atomic result publication. Sparse
  three-hourly wave height/period/direction records are selected within a
  bounded nearest-frame window on an hourly combined timeline, with their
  actual source time carried across the boundary and shown by the host.
- Environmental generator helper using versioned job/result/progress messages,
  the production GFS/HRRR/UKV/ICON/ECMWF weather, GFS/Copernicus wave, and
  Marine.ie/Copernicus/RTOFS/local/TPXO/offline-tidal current sources exposed by
  its capability document. Package metadata supplies requirements, coverage,
  limitations and deterministic long-range fallbacks. Completed output is
  independently inspected before atomic activation; failure never replaces
  the currently held valid dataset.
- Linux helper containment with bubblewrap namespaces and explicit immutable
  input/output/CA grants, `prlimit` CPU/address-space limits, child termination
  on cancellation/disable and deterministic cleanup. Windows uses kill-on-close
  Job Objects with process, memory, CPU and wall limits. macOS uses a
  deny-default `sandbox-exec` profile with exact paths and optional outbound
  network. Flatpak uses its outer application sandbox plus `prlimit`, exact
  document/private paths and a distinct target payload; these non-Linux-native
  paths require their CI execution evidence before support is claimed.
- Deterministic `.ocpnp` packages, complete SHA-256 file inventory, Ed25519
  signature verification, executable-path restrictions, malicious archive
  rejection, immutable extracted files, atomic replacement and retained
  rollback packages. The checked-in key is development-only.
- Reusable cross-platform conformance runner which reports pass/skip rather
  than extrapolating support.

## Linux x86-64 evidence

- Full Test-OpenCPN links with `-Werror`.
- Seventeen focused native tests pass: real component
  lifecycle/trap/identity and typed-service exercise (including concurrent
  compute replicas and cancellation); package/signing/update security tests;
  permission, broker, scheduler and declarative-UI tests; exact environment
  sampling and CM93 semantic-index tests; polar parsing; and iWeatherRouting
  package/UI/service conformance. The Rust runtime bridge passes three tests
  and the iWeatherRouting component passes sixteen solver tests.
- The exact final executable and signed packages were launched again in a
  fresh isolated `/tmp` profile. Both components loaded, the broker discovered
  iGRIB's environmental-provider service, and the schema-rendered
  iWeatherRouting window opened without affecting either installed OpenCPN.
- Signed package install and replacement preserve executable helper modes and
  retain the previous version in `.rollback`.
- A 44,865,184-byte real GRIB fixture was decoded as 465 messages, nine
  supported environmental field groups and 84 forecast times. At its 19:00
  hourly step, the installed GUI retained 62,910 samples and explicitly
  reported that its sparse wave height, peak-period and direction records came
  from 18:00. The bounded conformance frame retained 7,760 samples. Both the
  first and last of the 84 distinct forecast frames were decoded and verified.
  Timeline playback advanced and overlays remained responsive.
- A deterministic independent fixture exercises every one of the thirteen
  field groups, all pressure levels, temporal interpolation, multi-file
  ordering and weather-table output. Generated output must pass `codes_count`,
  `grib_ls` and a fresh decoder process; a forced failed regeneration is checked
  byte-for-byte not to replace the previous valid output.
- Malformed input returned a structured `environment-decode-failed` result and
  did not affect OpenCPN.
- The generator helper copied/validated the 44.8 MB fixture under containment
  and published a valid 387-message GRIB.
- The real versioned generator job path merged a 244-message local weather
  input and a 146-message local current input into a valid 44,815,544-byte,
  390-message output. The sandbox mounted only those two files read-only at
  fixed capability paths; no source directory was exposed.
- The installed Test-OpenCPN GUI repeated that merge using explicit **Local
  GRIB file…** weather/current source selections, automatically reopened the
  390-message output and retained 46,468 decoded samples for its first frame.
  The native xGRIB library was absent from the plugin search path during this
  final run.
- The actual GUI generated a one-degree, one-hour NOAA GFS product over HTTPS,
  wrote a 1,246-byte/six-message GRIB, reopened it and displayed 75 retained
  samples. An initial missing CA mount and wxJSON string-conversion bug were
  detected by this test and fixed before the final run.
- An authenticated live Copernicus Marine North-West Shelf request against
  `cmems_mod_nws_phy-cur_anfc_1.5km-2D_PT1H-i` generated and re-inspected a
  bounded two-message GRIB containing the expected `u_49` and `v_50` current
  components. The final host-rendered GUI test combined live GFS and
  Copernicus data into a valid 1,546-byte, ten-message GRIB, reopened it in
  iGRIB and retained no password in its job, arguments, settings or report.
  The opt-in conformance path also publishes no credentials.
- The component's host HTTP action downloaded 7,655 bytes and read them back
  through namespaced private storage.
- A four-segment chart coverage request completed as one batch. A deliberate
  Wasm trap was contained and OpenCPN continued operating.
- Final GUI testing ran with native xGRIB absent from the Test plugin search
  path. iGRIB therefore has no enabled or loadable native xGRIB provider.
- The installed Test-OpenCPN GUI exposed all 84 forecast steps as readable UTC
  dates and retained 46,468 first-frame samples. Accessibility inspection
  verified independent Wind, Pressure, Waves, Current and Air temperature
  settings pages, five colour selectors, meteorological wind barbs and
  proportional-current-arrow selection. The current display profile migrated
  to arrow-only defaults without affecting the optional magnitude overlay.
  The wave profile migrated from scalar spots to distinct crest-and-travel
  symbols while retaining the scalar map as an option. The display settings
  and last successfully opened GRIB directory survived a close/reopen cycle in
  the isolated profile.
- The host land classifier was checked against bundled shapefile data at
  Dublin, the Isle of Man and Manchester (land) and at an Irish Sea and an
  Atlantic point (water). Current and wave rendering/readouts are suppressed
  at classified land points; this display mask is explicitly not a
  hydrographic or navigation-safety result.
- Routing frame decoding now uses a headless `posix_spawn`/`waitpid` path from
  compute workers. It does not call synchronous `wxExecute`, which enters
  `wxWindowDisabler` and GTK from the wrong thread in a wxGUI application.
  The portable request carries minimum and maximum true-wind-angle bounds,
  separate optional true/apparent wind-speed and wave-height limits, explicit
  current/wave coverage policy, performance efficiencies, manoeuvre penalties
  and bounded search controls. The host validates and persists these settings;
  the component enforces them during expansion and rechecks the exact final
  approach before returning a route.
- iWeatherRouting acquires a private read-only snapshot with a revision,
  byte-size and SHA-256 identity. A real GRIB routing fixture mutates the
  original selected file after acquisition and proves route sampling continues
  against the unchanged held dataset.
- A clean Release build with `OCPN_ENABLE_PORTABLE_PLUGINS=OFF` completed and
  contained no portable runtime symbols or objects. The native loader files
  have no source diff.

Measured conformance values for this machine are recorded in
`conformance-linux-x86_64.md`. Values are observations, not release budgets.

## Still experimental or incomplete

- Windows x86-64, macOS Intel/Apple Silicon, Linux ARM64/Raspberry Pi and
  Flatpak were not executed on this local Linux machine. A required CI workflow
  now builds all seven target payloads, refuses platform-neutral payload drift,
  signs one archive and runs native/real-Flatpak conformance. Until the current
  workflow run is green, those rows remain `pending`, not inferred passes.
- The UI supplies functional xGRIB workflow/settings parity through a generic
  host renderer, not pixel-for-pixel inheritance of native wxWidgets. A future
  visual refinement must remain package-driven and must not move provider code
  back into core.
- Chart coverage batching is real, but structured land/depth/drying/conflict
  safety evidence and route-shaped immutable caches are not yet implemented.
- iWeatherRouting supports start/destination selection from live position,
  OpenCPN waypoint snapshots, chart cursor or manual coordinates. It loads
  bounded OpenCPN `.pol` tables and boat `.xml` manifests, transfers typed
  value grids, and interpolates actual TWS/TWA performance in Wasm; a
  conservative Nicholson 35 Mk1 model is bundled. Multi-table selection uses
  the fastest applicable table, while native crossover contours, ordered
  intermediate waypoints and OpenCPN route-object publication remain later,
  separately versioned services; exported GPX and overlays are implemented.
  Selected-route isochrones, route-to-cursor predecessor traces and boat
  interpolation at iGRIB's selected forecast time are implemented using
  bounded value geometry. Variable-departure results expose a selectable
  comparison table (including unsuccessful attempts and route/environment
  metrics); selection consistently drives the emphasized overlay, inspection
  layers and GPX output. Environmental availability for every requested
  departure is checked before compute workers start. Multi-time sampling
  groups route states by forecast instant, decodes immutable catalogue frames
  once and interpolates intermediate route times in-process. Cached immutable
  frames are sampled outside the decoder mutex so independent departure
  searches can progress concurrently. The viewer and router use byte-budgeted
  LRUs (256 MiB default,
  configurable from 32 MiB to 4 GiB) and the viewer prefetches its adjacent
  frame. A single frame larger than the budget remains valid as the sole cache
  entry, so the cache is not a GRIB-area limit. Provider diagnostics cross host
  ABI 9 as text instead of being reduced to an opaque numeric error.
- Production catalogue/TUF metadata, revocation, user permission-consent UI,
  cross-platform credential-store conformance, state migrations and a
  security-response ownership agreement remain absent.
- Guest entry dispatch and some host callbacks still need a dedicated serial
  runtime executor before production so no potentially long guest call can
  occupy the wx event thread.
- CPU and memory quotas are enforced for the component, Linux helpers and
  Windows Job Object helpers; per-plugin fair scheduling, hardened macOS code
  signing, Linux cgroups/seccomp and robust restart throttling remain production
  work.

## Next release gates

1. Require a green seven-target workflow and publish its signed multi-target
   archive plus reports; add GUI screenshots/manual checks on every host.
2. Add a Windows restricted token, hardened/notarised macOS helper and Linux
   seccomp/cgroup controls where deployable.
3. Move runtime entry to a serial supervisor executor and add shutdown/leak
   soak tests.
4. Add the stock-compatible S-57 backend and cross-platform CM93 fixtures to
   the host-owned chart-safety service, then add immutable dataset handles,
   crossover-aware multi-polar selection and waypoint sequences without
   changing the existing environmental batch contract.
5. Complete catalogue trust/revocation/consent and select an owned Wasmtime LTS
   before any production enablement.
