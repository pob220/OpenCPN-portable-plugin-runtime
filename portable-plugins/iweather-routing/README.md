# iWeatherRouting

iWeatherRouting is the second portable-plugin reference workload. Its bounded
three-stage route search executes inside the WebAssembly component: adaptive
forward isochrones first, destination-side reverse-isocrone bridge recovery
second, and a time-dependent position/time/heading/tack graph fallback last.
Every stage propagates forward through time and a route from any stage must pass
an independent chronological replay before it is returned. OpenCPN
brokers typed environmental samples published by iGRIB, batched chart checks,
cancellation, a declared four-tab/form control schema, GPX output and retained
route overlays. Forward departure comparisons run in up to four isolated
Wasmtime Stores. Results are planning aids and are not
navigation-authoritative.

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

Completed searches return bounded retained isochrones and exact predecessor
traces. The host can draw the selected route's isochrones, show the trace
nearest the chart cursor and interpolate a boat marker at iGRIB's displayed
forecast time. These are inspection aids, not evidence that unselected space
or a chart segment is safe.

The maximum-state setting bounds feasible labels retained after spatial,
tack, incoming-heading and propulsion reduction; transient heading candidates
do not consume that budget. Forward pruning is sector-balanced so a tack which
temporarily increases destination range is not erased by a purely greedy
ranking. Candidate fans include polar-derived optimum upwind/downwind VMG
laylines as well as configured TWA limits. Raw candidates are reduced before
the batched chart-coverage boundary while preserving local alternatives for
chart rejection.

The forward stage reserves bounded state capacity for recovery. Reverse
recovery ranks historical forward states from the destination side and tests
reproducible bridges directly and through destination-centred approach rings.
If no bridge survives, bounded time-dependent graph search explores position,
forecast time, heading, tack and propulsion labels in a 120 NM passage corridor. Its
remaining-time bound comes from the fastest configured polar/motor speed, not
a fixed boat speed. When currents are enabled, the request has no global upper
bound on favourable current, so the fallback correctly becomes bounded
Dijkstra rather than using a potentially inadmissible A* heuristic. Label
dominance preserves non-dominated elapsed-time/motor-use alternatives and
minimum-run state. A stationary wait action of at most six consecutive hours
allows the graph to wait for a forecast or tidal gate without consuming motor
or fuel. The progress bar explicitly identifies `Forward isochrone`, `Reverse-isocrone
recovery` and `Time-dependent graph fallback`, with retained/queued counters,
so a difficult calculation is distinguishable from a stalled component.

When the selected resolution is coarser than 30 minutes, 1.5 NM or 5 degrees,
the independently validated initial solution becomes a safe incumbent and the
component automatically reruns inside a 12 NM-or-wider route corridor at those
finer limits. Progress explicitly identifies this corridor-refinement pass and
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
chart-corridor validation. This reference remains a portable-runtime proof of
concept: it demonstrates a professional bounded solver cascade over typed host
services, but it is not a safety-certified navigator and does not import native
SuperCPN or legacy Weather Routing objects into Wasm.

When departure comparison is enabled, the Results tab opens automatically and
lists every attempted departure, including failures. It reports the best
(shortest elapsed) passage, offset, UTC departure/ETA, elapsed time, distance,
average boat speed and SOG, maximum SOG, wind/current statistics, tack count,
motor time/fuel/mode changes when propulsion is enabled, the existing Weather
Routing-style subjective comfort category, retained
isochrone count and state. Selecting any successful row immediately makes it
the emphasized chart route and changes its isochrones, cursor traces, forecast
time boat marker and GPX export; the other successful routes remain thin
comparison overlays.
