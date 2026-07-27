# OpenCPN Portable Plugin API 0.5 design

## Decision

OPP API 0.5 is the universal portable-plugin profile. It retains the complete
general-author surface introduced by OPP API 0.4 and adds first-class
environmental-data, bounded-compute, chart-safety and weather-routing
services. The runtime continues to load APIs 0.1 through 0.4 without
reinterpretation.

The API is universal in coverage, not authority. A package selects one
additive world and receives only the services allowed by its signed manifest
and approved permissions.

## Worlds

| Manifest world | General services | Specialist imports | Specialist exports |
| --- | --- | --- | --- |
| `plugin` | all | none | none |
| `environment-provider-plugin` | all | compute jobs, provider downloads, environmental dataset status, chart safety | job events |
| `weather-routing-plugin` | all | immutable environmental sampling, routing control, chart safety | single-leg route engine |
| `passage-weather-routing-plugin` | all | immutable environmental sampling, routing control, chart safety | single-leg and continuous-passage route engines |

`environment-provider-plugin`, `weather-routing-plugin` and
`passage-weather-routing-plugin` include `plugin-world`; they do not fork or
duplicate its semantics.

## Service ownership

### Guest-visible general services

- diagnostics and settings;
- context-rich actions and semantic surfaces;
- revisioned, paged navigation objects and confirmed mutations;
- retained multi-canvas scenes and input;
- private atomic storage and explicit user-file grants;
- timers, typed events, plugin messages and typed RPC;
- allowlisted HTTPS, communication endpoint discovery and validated
  NMEA output;
- host environment snapshots.

### Guest-visible specialist services

- bounded compute jobs with progress, completion, cancellation and failure
  events;
- provider downloads committed directly to package-private storage;
- immutable environmental snapshot identity and large bounded sample batches;
- host-owned chart coverage and final exact-geometry safety assessment;
- cooperative route progress and cancellation;
- single-leg and ordered-gate passage routing exports.

### Host-owned declarations and resources

Provider credentials, signed native helpers, decoded datasets and chart
objects never enter guest memory. They remain declared in the signed package
manifest and are mediated by the native host. This is part of OPP, not an
escape from OPP: the corresponding permissions, limits, helper protocols,
dataset generation identifiers and error semantics are versioned and tested
with the 0.5 profile.

## Performance contract

- Environmental calls are batch-first and capped at 100,000 samples.
- Each batch is evaluated against one immutable dataset generation.
- Chart safety is batch-first and capped at 10,000 segments.
- Exact delivered geometry must pass `query-final-safety`; advisory coverage
  cannot satisfy an authoritative request.
- Route computation uses a fresh isolated Wasmtime store and is bounded by
  retained states, fuel, memory, wall-clock deadline and cooperative
  cancellation.
- Native helper data is exchanged through bounded files or handles rather
  than copied repeatedly through component memory.
- Implementations may parallelise independent departures, but a package
  generation owns and revokes every job, timer, subscription and route call.

## Safety invariants

1. Chart objects and provider credentials are never exposed to Wasm.
2. Missing, stale or non-authoritative safety evidence is distinguishable
   from safe water.
3. A route requested with authoritative safety fails closed.
4. Final route validation uses fresh environmental samples and exact segment
   geometry.
5. Disabling, replacing or failing a package revokes all dynamic authority.
6. OPP 0.5 does not weaken the API 0.2 weather-routing safety semantics.

## Compatibility and migration

| Package | Current profile | OPP 0.5 target | Required parity gate |
| --- | --- | --- | --- |
| iPolars | OPP 0.4 `plugin` | optional OPP 0.5 `plugin` | editor and scoped-file parity |
| iGRIB | API 0.2 `plugin` | OPP 0.5 `environment-provider-plugin` | helper, provider, dataset and workbench parity |
| iWeatherRouting | API 0.2 `passage-weather-routing-plugin` | OPP 0.5 `passage-weather-routing-plugin` | route quality, exact chart safety, environmental replay and cancellation parity |

API 0.2 packages remain installable during migration. No package is relabelled
as 0.5 until its component, bundled WIT contract, signed archive and installed
runtime execution all pass the corresponding parity gate.

## Rejected alternatives

- Adding specialist services to frozen OPP 0.4.
- Tunnelling environmental or chart-safety calls through generic plugin RPC.
- A monolithic world which grants every package every service.
- Splitting one plugin into a 0.4 UI component and a 0.2 engine component.
- Treating helper executables or host C++ objects as guest-callable ABI.
