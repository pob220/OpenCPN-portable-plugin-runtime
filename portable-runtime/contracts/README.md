# Portable API contracts

`0.1` remains the production-compatible world in `portable-runtime/wit`.
Existing signed packages continue to load against it.

`0.2` begins the additive, modular API. Its lifecycle, filtered event stream,
and plugin-message interfaces are separate so a future host can negotiate and
grant them independently. The runtime's bounded capability event broker is
the transport shared by the `0.1` compatibility adapter and the future typed
`0.2` adapter.

`chart-safety` is a host-owned, renderer-independent service. Portable route
engines submit value-only segment geometry and vessel clearance requirements;
the host retains all chart objects and returns typed land, drying, depth and
coverage assessments. A coastline fallback is explicitly distinguishable
from authoritative vector/CM93 chart semantics.

The contract is compiled by the Rust bridge test build. It is not yet selected
by installed packages; moving a package to `portable_api >=0.2.0` will happen
only after the host supports side-by-side `0.1` and `0.2` instantiation.
