# OpenCPN Portable Plugin API contracts

`0.1` remains the compatibility world in `portable-runtime/wit`. The manager
selects it from the signed manifest and a dedicated legacy guest is exercised
by the bridge smoke test.

`0.2` is the additive, modular API used by the reference iGRIB, iPolars,
iWeatherRouting and capability-lab packages. Host services, lifecycle,
filtered events, plugin messages and callback sinks are separate interfaces.
Most packages select `plugin-world`; route engines select the strictly larger
`weather-routing-plugin-world`. Engines which preserve tactical state through
an ordered collection of pass-through gates select the additive
`passage-weather-routing-plugin-world`; the original single-leg export remains
present for compatibility. The runtime's bounded capability event broker feeds
both the `0.1` compatibility adapter and the typed `0.2` sinks.

`0.3` is the general plugin-author profile. It adds stateful toolbar and chart
context actions, host-owned declarative surfaces, scoped files, bounded
navigation-object access, retained multi-primitive scenes, pointer/key input,
timers, validated NMEA output, dynamic subscriptions and asynchronous typed
plugin RPC. Dynamic subscriptions are owned by the enabled package generation
and are revoked on disable, failure or unload.

`0.4` is the first contract named **OpenCPN Portable Plugin API (OPP API)**.
Its WIT namespace is `opencpn:opp@0.4.0`. It adds typed event variants,
context-rich actions, host environment snapshots, semantic surface roles,
revisioned/paged navigation objects, multi-canvas/render-phase scenes,
package-scoped communication endpoints, allowlisted NMEA 2000 output and
controlled HTTPS. The 0.1–0.3 contracts remain frozen.

`chart-safety` is a host-owned, renderer-independent service. Portable route
engines submit value-only segment geometry and vessel clearance requirements;
the host retains all chart objects and returns typed land, drying, depth and
coverage assessments. A coastline fallback is explicitly distinguishable
from authoritative vector/CM93 chart semantics.

The complete versioned WIT directory is shipped in each reference package.
Installed manifests select exactly one compatible major-minor range:
`>=0.1.0 <0.2.0`, `>=0.2.0 <0.3.0`, `>=0.3.0 <0.4.0`, or
`>=0.4.0 <0.5.0`. API `0.2` and later manifests must declare their world. The
bridge keeps all four versions side by side; OPP API 0.4 does not reinterpret
an installed component.
