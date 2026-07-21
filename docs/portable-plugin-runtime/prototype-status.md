# Portable runtime implementation status

Status date: 2026-07-20. Baseline: OpenCPN Release 5.14.0 in the separate
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
  time-indexed environmental sample batches, cancellable weather-routing,
  chart-coverage batches, host HTTP-to-private-storage and bounded private
  reads. No pointers, wxWidgets objects or graphics handles cross the boundary.
- Manifest-declared, versioned service discovery connects iGRIB's
  `org.opencpn.environment.provider` implementation to iWeatherRouting without
  native-plugin broadcast strings. Missing providers fail closed.
- iWeatherRouting's adaptive time-layer route search executes in the portable
  Rust component. The host supplies a schema-validated declarative UI whose
  tabs and required form controls are package metadata, typed
  iGRIB samples, batched GSHHS checks, cancellation, progress, retained route
  alternatives and user-selected GPX output. A bounded departure window uses
  up to four independent Wasmtime Stores concurrently and selects the earliest
  safe arrival. Route endpoints can be resolved from the live vessel position,
  stable value snapshots of OpenCPN waypoints, the latest chart cursor or
  manual coordinates; no core navigation object crosses the boundary.
- Host-rendered declarative iGRIB surface with readable UTC timeline
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
- Standalone ecCodes helper supporting bounded `inspect` and `frame` operations
  with structured result/error JSON and atomic result publication. Sparse
  three-hourly wave height/period/direction records are selected within a
  bounded nearest-frame window on an hourly combined timeline, with their
  actual source time carried across the boundary and shown by the host.
- Environmental generator helper using versioned job/result/progress messages,
  GFS and UKMO providers, optional waves and authenticated Copernicus Marine
  North-West Shelf or global current inputs, optional local weather/current
  GRIB replacement inputs, user-selected output and automatic reopening of
  completed output.
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
- Five CTests pass: real component lifecycle/trap/identity and typed service
  exercise (including four concurrent compute replicas and cancellation);
  package/signing/update security tests; beta-tool security checks; iGRIB
  target package/helper conformance; and iWeatherRouting package/UI/service
  conformance.
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

Measured conformance values for this machine are recorded in
`conformance-linux-x86_64.md`. Values are observations, not release budgets.

## Still experimental or incomplete

- Windows x86-64, macOS Intel/Apple Silicon, Linux ARM64/Raspberry Pi and
  Flatpak have not been built or executed in this session. Their helper
  binaries, signing/notarisation and OS-specific process controls remain gates.
- The package assembled here contains the Linux x86-64 helper payload. A single
  multi-target archive requires the central build service to add all signed
  target helpers; the Wasm component itself is unchanged across targets.
- The UI is an xGRIB-style functional surface with the core bundled-GRIB
  display workflow, not a pixel-for-pixel port of every xGRIB preference,
  particle-map mode or provider dialog. Broader parity should be incremental,
  not an expansion of the WIT boundary into wxWidgets.
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
  decodes and consumes frames incrementally so a route-statistics batch cannot
  evict its own frames from the bounded cache; provider diagnostics cross host
  ABI 9 as text instead of being reduced to an opaque numeric error.
- Production catalogue/TUF metadata, revocation, user permission-consent UI,
  cross-platform credential-store conformance, state migrations and a
  security-response ownership agreement remain absent.
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
4. Replace the prototype GSHHS screen with structured chart-safety evidence,
   then add immutable dataset handles, crossover-aware multi-polar selection
   and waypoint sequences without changing the existing environmental batch
   contract.
5. Complete catalogue trust/revocation/consent and select an owned Wasmtime LTS
   before any production enablement.
