# Portable API contracts

`0.1` remains the compatibility world in `portable-runtime/wit`. The manager
selects it from the signed manifest and a dedicated legacy guest is exercised
by the bridge smoke test.

`0.2` is the additive, modular API used by the reference iGRIB, iPolars,
iWeatherRouting and capability-lab packages. Host services, lifecycle,
filtered events, plugin messages and callback sinks are separate interfaces.
Most packages select `plugin-world`; route engines select the strictly larger
`weather-routing-plugin-world`. The runtime's bounded capability event broker
feeds both the `0.1` compatibility adapter and the typed `0.2` sinks.

Subscriptions remain signed manifest declarations. `events.wit` reserves the
shape of a future dynamic subscription service, but it is intentionally not
imported by either current world until ownership, revocation and shutdown
semantics are finalised.

`chart-safety` is a host-owned, renderer-independent service. Portable route
engines submit value-only segment geometry and vessel clearance requirements;
the host retains all chart objects and returns typed land, drying, depth and
coverage assessments. A coastline fallback is explicitly distinguishable
from authoritative vector/CM93 chart semantics.

The complete versioned WIT directory is shipped in each reference package.
Installed manifests select either `>=0.1.0 <0.2.0` or
`>=0.2.0 <0.3.0`; API 0.2 manifests must also declare their world.
