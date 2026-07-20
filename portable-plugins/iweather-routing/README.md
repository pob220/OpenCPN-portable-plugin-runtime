# iWeatherRouting

iWeatherRouting is the second portable-plugin reference workload. Its adaptive
time-layer route search executes inside the WebAssembly component. OpenCPN
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

The host-rendered route form can resolve start and destination values from the
live vessel position, OpenCPN waypoints (copied as stable GUID/name/coordinate
records), the latest chart cursor position, or manually entered coordinates.
No native navigation object or pointer crosses the component boundary.
