# OPP API 0.5 author guide

OPP API 0.5 is the universal OpenCPN Portable Plugin author profile. Its WIT
namespace is `opencpn:opp@0.5.0`; a package selects the exact compatible
minor version and the smallest world which contains its required services:

```json
{
  "portable_api": ">=0.5.0 <0.6.0",
  "portable_world": "plugin"
}
```

The authoritative sources are in `contracts/0.5`. All four worlds include the
same general lifecycle and author services:

- `plugin` — actions, surfaces, navigation, scenes, files, timers,
  communications, typed events, RPC and controlled HTTPS.
- `environment-provider-plugin` — the general world plus supervised compute
  jobs, provider downloads, chart coverage and environment-dataset status.
- `weather-routing-plugin` — the general world plus immutable batched
  environmental sampling, routing progress/cancellation and chart safety.
- `passage-weather-routing-plugin` — the routing world plus continuous,
  ordered multi-gate passage calculation.

World selection is least authority, not a statement that the other services
are absent from OPP. A guest receives only the imports declared by its world
and only the permissions approved from its signed manifest.

## Safety and performance boundaries

- Environmental samples are evaluated in bounded batches against one
  immutable host-owned dataset generation. Provider replacement cannot split
  a call across generations.
- Chart objects and credentials never enter guest memory. Route engines pass
  value-only geometry to `chart-safety`.
- Search-time chart probes are provisional. A route must still pass the
  independent `query-final-safety` boundary before delivery.
- Provider networking writes atomically into package-private storage and
  never discloses host credentials.
- Every guest entry has bounded memory, fuel and wall-clock execution. Disable,
  failure, update and unload revoke generation-owned actions, jobs, timers,
  subscriptions and file grants.

## Compatibility

API 0.1–0.3 and OPP API 0.4 remain frozen and load side by side. Rebuild a
0.4 general plugin only when it needs 0.5; there is no forced reinterpretation.
Migrate older environment and routing plugins to 0.5 so they can combine the
general author surface with their specialist services without losing
functionality.
