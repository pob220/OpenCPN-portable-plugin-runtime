# iWeatherRouting

iWeatherRouting is the second portable-plugin reference workload. Its adaptive
time-layer route search executes inside the WebAssembly component. OpenCPN
brokers typed environmental samples published by iGRIB, batched chart checks,
cancellation, a declared four-tab/form control schema, GPX output and retained
route overlays. Forward departure comparisons run in up to four isolated
Wasmtime Stores. Results are planning aids and are not
navigation-authoritative.

The vessel/safety surface follows the SuperCPN constraint model where minimum
and maximum true-wind **angles** are distinct from optional true/apparent
wind-speed limits. It also exposes wave/current coverage policy, maximum wave
height and latitude, polar efficiency, tack/gybe time, search angle,
destination tolerance and bounded departure parallelism. Invalid ranges are
rejected before a portable route job starts and settings persist per plugin.

The host-rendered route form can resolve start and destination values from the
live vessel position, OpenCPN waypoints (copied as stable GUID/name/coordinate
records), the latest chart cursor position, or manually entered coordinates.
No native navigation object or pointer crosses the component boundary.
