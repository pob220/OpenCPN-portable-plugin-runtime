# iWeatherRouting

iWeatherRouting is the second portable-plugin reference workload. Its OPP API
0.5 continuous-passage world combines the complete general author surface with
immutable environment batches, routing control and chart safety. Its bounded
three-stage route search executes inside the WebAssembly component: adaptive
forward isochrones first, destination-side reverse-isocrone bridge recovery
second, and a time-dependent position/time/heading/tack graph fallback last.
Every stage propagates forward through time and a route from any stage must pass
an independent chronological replay before it is returned. OpenCPN
brokers typed environmental samples published by iGRIB, batched chart checks,
cancellation, a declared four-tab/form control schema, GPX output and retained
route overlays. Symmetric departure-time optimisation searches before and
after the nominal time, nearest alternatives first, in up to four isolated
Wasmtime Stores subject to the host's hardware limit. Results are planning
aids and are not navigation-authoritative.

Routes use a real tabulated vessel model. The host accepts either an OpenCPN
weather-routing `.pol` table or an OpenCPN boat `.xml` file referencing up to
eight polar tables, validates and bounds it, and passes only typed numeric
grids to the component. The component performs bilinear TWS/TWA interpolation
for every candidate and uses the fastest applicable table. XML references are
restricted to the selected boat directory or its sibling `polars` directory;
native objects and file paths never cross the Wasm boundary. The package
includes a conservative Nicholson 35 Mk1 cruising polar as a usable default.
Native crossover-contour state is deliberately not imported in this first
model.

The vessel/safety surface follows the SuperCPN constraint model where minimum
and maximum true-wind **angles** are distinct from optional true/apparent
wind-speed limits. It also exposes wave/current coverage policy, maximum wave
height and latitude, polar efficiency, tack/gybe time, search angle,
destination tolerance and bounded departure parallelism. Invalid ranges are
rejected before a portable route job starts and settings persist per plugin.
The selected boat/polar path persists too; it is revalidated on every startup.
New installations use a conservative 300-second tack and gybe loss, matching
the five-minute real-boat manoeuvre increment used by qtVlm rather than
pretending there is one empirical average for all yachts. Both values remain
editable down to zero and saved values always take precedence over defaults.

Optional propulsion is an explicit, default-off route policy executed by the
portable component rather than a cosmetic host setting. Users may independently
allow motor-sailing and motor-only operation, select the sailing-speed crossover,
motor speed, motor-sailing boost, crossover hysteresis, minimum run time and
mode-change penalty, and enforce passage-wide motor-time and fuel budgets. The
search carries propulsion mode and consumed motor time in each retained state;
the independent final-route replay applies the same policy and budgets. Results
report motor time, estimated fuel and propulsion transitions, including for
multi-leg OpenCPN routes. Fuel values are estimates derived from the configured
constant consumption rate, not tank or engine instrumentation.

The host-rendered route form can resolve start and destination values from the
live vessel position, OpenCPN waypoints (copied as stable GUID/name/coordinate
records), the latest chart cursor position, or manually entered coordinates.
No native navigation object or pointer crosses the component boundary.
An existing OpenCPN route can also be loaded directly from the workbench's
**Routings** menu. On stock OpenCPN 5.14, right-clicking a route on the chart
offers **Weather Route Analysis…** while iWeatherRouting is enabled. Both
entry points copy the selected route as value records and preserve every
waypoint, in order, as a mandatory routing gate; two-point routes use the same
workflow without an unnecessary multi-leg distinction.

Chart safety is a host-owned value service rather than a renderer or modified
OpenCPN-core dependency. On stock OpenCPN 5.14 the Portable Plugin Manager
indexes configured CM93 roots with its own bounded, read-only semantic decoder.
Search-time propagation rejects `LNDARE`, `DRGARE` and `ITDARE` geometry; dense
final replay additionally requires `DEPARE/DRVAL1` coverage at the configured
minimum depth. Decoded cells and cell lookups are immutable and cached. The
manager runs semantic decoding on routing workers rather than the GUI thread,
uses exact CM93 cell-grid traversal and rate-limits file metadata checks.
Only unresolved advisory GSHHS fallbacks are dispatched to OpenCPN's main
thread. The component expands each final segment into a five-line swept
corridor, so the centre, parallel clearances and crossing diagonals are all
checked. If the user requires authoritative chart safety, absent or incomplete
semantic/depth coverage fails closed. GSHHS is retained only as an explicitly
advisory fallback when authoritative validation is disabled.

Completed searches return bounded retained isochrones and exact predecessor
traces independently of their current visibility. The Runtime Host captures a
fine outer-front representation for every completed solver layer, so both
overlays can be toggled after calculation without repeating the route. For a
multi-waypoint passage, traces on later legs are prefixed with the completed
earlier legs and therefore run from the overall passage origin to the selected
front. The host can draw the selected route's isochrones, show the trace nearest
the chart cursor and interpolate a boat marker at iGRIB's displayed forecast
time. These are inspection aids, not evidence that unselected space or a chart
segment is safe.

Isochrone presentation is independent of solver resolution. **Navigation**
automatically thins minor fronts according to passage duration and chart scale,
**Analysis** exposes every retained layer, and **Minimal** keeps major,
selected-GRIB-time and final fronts. A custom display can set its interval,
major-contour interval, width, opacity, uniform or elapsed-time colour sequence,
time labels, cursor-focus fading and diagnostic front points. Repeated
equal-time components remain disconnected, major contours are emphasized, and
the closest retained contour to iGRIB's displayed time is highlighted. These
settings persist per package and never alter, discard or recompute routing
states. Day, dusk and night palettes use the same display plan in software,
legacy OpenGL and the wxDC chart frame presented by OpenCPN 5.15's Vulkan
renderer.

The maximum-state setting bounds feasible labels retained after spatial,
tack, incoming-heading and propulsion reduction; transient heading candidates
do not consume that budget. Forward pruning is sector-balanced so a tack which
temporarily increases destination range is not erased by a purely greedy
ranking. Candidate fans include polar-derived optimum upwind/downwind VMG
laylines as well as configured TWA limits. Raw candidates are reduced before
the batched chart-coverage boundary while preserving local alternatives for
chart rejection. The reduction is streamed into the exact best-N cell sets:
discarded `Node` values never receive clearance geometry and do not remain in a
second transient candidate arena. Hot-loop environmental, draft and chart
buffers are reused between forecast layers.

The forward stage reserves bounded state capacity for recovery. Reverse
recovery ranks historical forward states from the destination side and tests
reproducible bridges directly and through destination-centred approach rings.
Long forward searches yield bounded state tranches to reverse recovery and,
when necessary, the graph, then resume their retained frontier. An early
recovery route is an incumbent rather than a guessed ETA bound: forward search
continues until its chronological frontier reaches the incumbent arrival time
or its declared state tranche ends. This remains valid with an arbitrarily
strong favourable current because no polar-STW-to-COG assumption is used.
If no bridge survives, bounded time-dependent graph search explores position,
forecast time, heading, tack and propulsion labels in a 120 NM passage corridor. Its
remaining-time bound comes from the fastest configured polar/motor speed, not
a fixed boat speed. When currents are enabled, it adds the portable
environment service's conservative physical current ceiling, preserving an
admissible A* heuristic without assuming a typical tidal speed. Label
dominance preserves non-dominated elapsed-time/motor-use alternatives and
minimum-run state. A stationary wait action of at most six consecutive hours
allows the graph to wait for a forecast or tidal gate without consuming motor
or fuel. The progress bar explicitly identifies `Forward isochrone`, `Reverse-isocrone
recovery` and `Time-dependent graph fallback`, with retained/queued counters,
so a difficult calculation is distinguishable from a stalled component.

When the selected resolution is coarser than 30 minutes, 1.5 NM or 5 degrees,
the independently validated initial solution becomes a safe incumbent and the
component automatically reruns inside a 12 NM-or-wider route corridor at those
finer limits. The fine pass can improve that route but does not need to
rediscover it or search chronologically beyond its arrival merely to retain a
valid result. Progress explicitly identifies this corridor-refinement pass and
the result reports the ETA difference as a resolution-sensitivity check. If
the finer bounded pass fails, the validated initial route is retained. Each
pass has its own configured state bound.

Current-aware replay distinguishes course over ground from vessel heading:
the independently sampled current vector is removed from each delivered leg
before applying polar, true-wind-angle and apparent-wind policy. Arrival legs
retain the physically propagated closest-approach point within the configured
destination tolerance rather than snapping a current-displaced track onto the
waypoint.

Weather used to admit a forward leg is sampled at its predicted midpoint,
matching the independent replay boundary. This removes the former failure mode
where start-of-hour wind admitted a leg which midpoint wind then rejected after
a forecast shift. Final replay additionally subdivides route geometry to at
most 15-minute and approximately 1.5 NM probes for environmental-limit and
chart-corridor validation; its odd probe count always includes the exact
original-leg midpoint later used for metrics. iGRIB decodes immutable
catalogue frames once and performs deterministic host-side interpolation for
intermediate route times, avoiding a full GRIB decode at every 15-minute
probe. Spatial sampling reuses exact conservative candidate-bin plans shared
by route states in the same GRIB grid cell. Immutable GRIB samples and CM93
segment assessments use deterministic output slots with a globally bounded
helper budget: 1–2 GiB systems get at most one helper for a lone caller, while
larger systems remain capped at four normal caller/helper slots and concurrent
departure searches reduce helper availability. CM93 cells also spatially index
hazard and depth/coverage areas without changing polygon tests or fail-closed
semantics. Final progress explicitly reports authoritative chart/depth
validation, and a rejection identifies land, drying area, shallow `DEPARE`, or
missing depth evidence. This reference remains a portable-runtime proof of
concept: it
demonstrates a professional bounded solver cascade over typed host services,
but it is not a safety-certified navigator and does not import native SuperCPN
or legacy Weather Routing objects into Wasm.

When departure comparison is enabled, the Results tab opens automatically and
lists every attempted departure, including failures. A host-owned coordinator
retains rows in chronological order while scheduling the nominal departure
first, applies the CPU/memory-safe worker limit, isolates a trapped candidate,
gives every cancelled candidate a terminal state and deterministically selects
only a complete independently validated passage. The configured range is
applied on both sides of the selected time and each live row reports queued,
preflight, forward-isocrone, reverse-recovery, graph-fallback, validation,
complete, failed or cancelled progress as appropriate.

The initial table is deliberately compact: best marker, offset, departure, ETA,
elapsed time, distance and state. Selecting a successful row exposes average
boat speed and SOG, maximum SOG, wind/current statistics, tack count, motor
time/fuel/mode changes, comfort category, retained isochrones, examined states
and validation counts in an expandable details panel. It also immediately
makes that passage the emphasized chart route and changes its isochrones,
cursor traces, forecast-time boat marker and GPX export; other successful
routes remain thin comparison overlays.

The optional stability corridor is calculated on demand and cached for the
current result set. It requires at least three complete independently validated
candidates, excludes routes outside the configured elapsed-time penalty,
resamples and clusters geometrically similar routes into distinct families,
then analyses only the family containing the selected row. The outer and inner
bands show configurable 40% and 70% route-family agreement by default. Every
agreement cell is batch-checked through the manager-owned chart-safety service;
unsafe or unresolved cells fail closed and are omitted. A medoid
representative route is drawn with the bands. The same renderer-neutral cell
plan is consumed by software, OpenGL and Vulkan-presented overlays.
