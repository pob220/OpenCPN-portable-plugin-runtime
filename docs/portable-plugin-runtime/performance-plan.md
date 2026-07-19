# Performance and feasibility plan

## Purpose

Performance tests decide interface shapes and placement; they are not a post-design optimisation exercise. Compare native/internal baselines, pure Wasm operations, WIT host batches and helper RPC on the same data/hardware. Publish raw results, build ids, compiler/runtime configuration and confidence intervals.

No threshold below is a safety guarantee. Initial numbers are **review gates to calibrate on reference hardware**, not promises.

## Risks to test first

1. millions of boundary crossings during route expansion;
2. copying/allocating large WIT lists under the current canonical ABI;
3. chart operations which must prepare data on the wx/main thread;
4. overlay scene validation/projection/upload flooding the UI frame;
5. Wasmtime cold compilation, binary-size and per-instance memory cost;
6. cancellation which cannot pre-empt a blocking host callback;
7. ARM64/Raspberry Pi thermal/memory constraints;
8. helper startup, RPC copies and Flatpak execution restrictions;
9. high-level guest runtime/component size and startup;
10. cache invalidation causing latency spikes.

The Component Model roadmap acknowledges aggregate copy/allocation friction and proposes a future lazy ABI. Design handles/batches now and do not depend on that future optimisation ([ABI roadmap](https://bytecodealliance.org/articles/the-road-to-component-model-1-0)).

## Reproducibility

Record OpenCPN commit/config, Wasmtime exact commit/features/backend, guest toolchain and optimisation, component/package digest, OS/kernel/Flatpak runtime, CPU/governor/core count, memory, GPU/renderer, chart/environment fixture digest and warm/cold cache state. Pin inputs. Use release builds with debug symbols retained separately. Run enough iterations for median, p95/p99 and dispersion; randomise candidate order; separate compile/cache/setup/query time.

Use one logical API and generated fixtures across:

- Windows x86-64;
- macOS x86-64 and Apple Silicon;
- Linux x86-64 and AArch64;
- representative 64-bit Raspberry Pi;
- Flatpak x86-64 and AArch64.

CI may run reduced fixtures; scheduled/release hardware runs full macrobenchmarks. Store machine-readable results and compare against a reviewed baseline with noise bands, not a brittle single percentage.

## Microbenchmarks

| Benchmark | Variants | Measurements / design decision |
|---|---|---|
| engine/component cold start | engine create; validate/compile; cache hit; instantiate/init; 1/10 components | wall/CPU, peak/RSS, cache bytes; lazy engine and cache policy |
| empty host call | sync/async; scalar/record/result; 1–1M calls | ns/call and CPU; minimum useful batch size |
| WIT lists | `f64`, records, strings at 1 KiB–256 MiB | throughput, allocations, peak memory; list vs packed buffer/stream |
| resource handle | create/borrow/drop, invalid/stale/cross-plugin | latency/table memory; handle pooling/lifetime |
| stream | 4 KiB–4 MiB chunks, backpressure/cancel | throughput, memory plateau, completion latency; chunk defaults |
| helper RPC | pipes vs candidate Cap'n Proto; copy and optional sealed shared buffer | latency/throughput/CPU; whether dependency/shared memory is justified |
| fuel/epoch | compute loop and mixed host calls | overhead, interrupt/cancel p95; quota configuration |
| memory limiter | grow/deny/drop/reinstantiate | RSS recovery and failure latency; per-store budgets |
| scene validation | primitive/node/string/geometry scale | validation/update/render CPU and memory; quotas/packed geometry |
| UI events | action/form event round trip under load | p95 latency and main-thread occupancy; rate/coalescing |

Never benchmark guest-provided precompiled Wasmtime images as an install optimisation: unsafe deserialisation of untrusted precompiled artifacts can permit native code execution ([Wasmtime precompilation warning](https://docs.wasmtime.dev/examples-pre-compiling-wasm.html)). A host-created, digest/runtime-bound cache is valid.

## xGRIB-style macrobenchmarks

Fixtures should include a small coastal file, a multi-day regional file, a large multi-field/global-style dataset, combined weather/wave/current sources, malformed/truncated/bomb inputs and deterministic synthetic fields.

Scenarios:

1. download over controlled HTTP with proxy/TLS/redirect, progress and cancel at 10/50/90%;
2. open/metadata/timeline without reading unused fields;
3. sample 1k, 100k and 1M `(position,time)` rows across 1/4/12 fields;
4. compare individual calls, WIT batches, packed columns and host vector implementation;
5. stream contour/arrow data and update a retained overlay at pan/zoom/timeline rates;
6. combine/regrid/merge and write output through ecCodes/NetCDF/PROJ helper;
7. helper crash, hang, malformed JSON/protocol and resource exhaustion;
8. concurrent consumer (Weather Routing) while viewer timeline updates;
9. plugin disable/re-enable and cache reuse/invalidation.

Measure time to first metadata/overlay, samples/sec, decoded/encoded MiB/sec, boundary bytes/copies, peak/RSS by process, main-thread ms/frame, dropped frames, helper startup/restart, progress cadence, cancel-to-quiescence and cleanup/RSS recovery.

## Weather Routing-style macrobenchmarks

Use fixed scenarios derived from `routing-engine-refactor` headless cases: short coastal, ocean, multi-waypoint/multi-leg, multiple departures, currents, adverse final approach/reverse recovery, chart/no-chart/missing-coverage, alternatives and stability corridor.

For each scenario record search states/edges, environmental rows/fields, safety segments, cache preparation geometry, overlay nodes and result quality checksum. Compare:

- native/refactored engine baseline;
- Rust component compute with analytic in-guest environment (compute ceiling);
- component plus host `sample-batch` at batch sizes 64–65,536;
- component plus chart `validate-segments` and route-shaped prepared cache;
- helper RPC alternative only if specialist native code is involved.

Measure total/departure-candidate latency, states/sec, samples/sec, segments/sec, time and bytes per boundary, chart preparation/main-thread occupancy, worker chart calls (must be zero after preparation for the relevant profile), progress/UI latency, cancel p50/p95, peak memory, deterministic result equivalence and restart after trap.

Do not optimise a different algorithm into a misleading faster answer. Validate route/result invariants and compare structured diagnostics, not only elapsed time.

## Overlay and UI workload

Scenes: 100/10k/100k line vertices; 1/10/100 alternatives; corridor polygons; 1k labels/icons; contour-style multi-polygons. Operations: full replace, 1%/10% patch, timeline swap, viewport pan/zoom, hit-test burst and disable cleanup. Test both DC and GL where available, plus the test setup's `--no_opengl` path.

Measure submit/validate/commit time, bytes and allocations, render CPU/GPU frame time, main-thread stall, update-to-visible latency and event round trip. Coalesce obsolete scene revisions and enforce a per-frame apply budget; a plugin cannot force immediate redraw for every chunk.

Declarative UI tests cover action activation, form validation, table updates and progress events under simultaneous compute. Target normal interaction p95 below one frame where scheduling permits; never let guest callbacks execute on the wx thread.

## Cancellation and failure tests

Cancellation injection points: before dispatch, during component CPU loop, while awaiting host batch, mid-HTTP/body stream, mid-helper decode/write, during scene transaction, during state migration and during shutdown. Record request-to-guest observation, host-operation stop, event-stream completion, resource release and UI recovery.

Provisional gates on representative desktop hardware:

- UI thread must never wait synchronously for guest compute/I/O;
- ordinary action/event p95 should remain under 50 ms at moderate load;
- cooperative compute cancellation p95 under 250 ms and hard instance interruption under 1 s;
- helper hard termination and resource reaping under 2 s;
- shutdown should finish cleanly under 2 s or force-drop/terminate without hanging OpenCPN;
- no unbounded RSS growth over 100 enable/run/cancel/disable cycles;
- deliberate trap, invalid output, helper crash and quota denial leave OpenCPN responsive.

Raspberry Pi may need different time gates but not weaker boundedness or cleanup.

## Provisional throughput and product gates

Calibrate native service baselines first. Proposed acceptance criteria:

- batch environmental/safety throughput at least 70% of the same host vector service invoked internally, or documented end-to-end routing overhead below 15%;
- boundary-call count grows with batches/chunks, not route-state count; macro scenario has no million-call pattern;
- packed format is adopted only if it improves a representative macro workload by at least 20% or halves peak copy memory, with no semantic loss;
- incremental scenes are adopted only if full replace misses frame/main-thread budgets for realistic routes/contours;
- helper is justified only when it materially improves dependency feasibility/isolation or achieves acceptable throughput unattainable in the component;
- linked compressed/uncompressed binary increase and cold-start/RSS are reported to release maintainers, who set the final budget before production;
- no target is called supported until its conformance and workload subset passes.

These gates force an architectural review, not automatic rejection: a failure may change batch shape, ownership or placement.

## Instrumentation

Add opt-in structured spans with package id (not secrets), service/op, batch rows/bytes, queue/execute/copy time, cancellation/deadline result and allocation/resource counts. Correlate component, service and helper using a random job id. Export benchmark JSON without precise user navigation data. Production logs sample/rate-limit and redact URLs/credentials/route positions.

Use runtime profiling where supported, native profilers for OpenCPN/helper, allocation counters and UI frame instrumentation. Wasmtime DWARF debugging is currently lower-tier/best-effort, so conformance tooling must not depend on identical debugger support everywhere ([Wasmtime tiers](https://docs.wasmtime.dev/stability-tiers.html)).

## Decision experiments

| Question | Experiment | Decision |
|---|---|---|
| WIT lists or packed columns? | 100k/1M environmental and segment batches across x86/ARM | choose simplest shape meeting throughput/memory gates |
| in-Wasm or helper codecs? | build/size/start/decode/write/fuzz comparison for ecCodes/NetCDF/PROJ subset | placement per operation, not ideology |
| Cap'n Proto dependency? | implement minimal framed protocol and Cap'n Proto spike with identical job | adopt only for measured/safety/maintenance advantage |
| JIT/cache/Pulley? | cold/warm/start/compute/binary size and macOS signing on targets | target policy; component package remains unchanged |
| full vs incremental scene? | route/corridor/contour full replacements and patches | retain both only if real threshold exists |
| in-process runtime enough? | traps/DoS plus runtime/helper operational comparison | optionally add out-of-process component mode later if risk warrants |

## Reporting

Stage 4 output is a versioned benchmark report, raw JSON, fixture digests, dashboards, regressions by release, and a conformance subset third parties can run. Separate: technically runnable; prototype gate passed; production gate passed; untested. Never extrapolate x86-64 results to Raspberry Pi/ARM64 or native results to Flatpak.
