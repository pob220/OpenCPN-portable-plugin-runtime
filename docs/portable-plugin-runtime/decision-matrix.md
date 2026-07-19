# Architecture decision matrix

## Method

Ratings: **5 excellent**, **4 good**, **3 workable with material constraints**, **2 weak**, **1 unsuitable**. Confidence is H/M/L. `P` means practical for prototype, `Prod` production-suitable after stated gates, `F` technically feasible but not recommended, and `Prem` desirable but premature.

Candidates:

- **CM** — in-process WebAssembly Component Model/WIT with Wasmtime only.
- **OOP** — local RPC to platform-native plugin executables.
- **Script** — embedded JavaScript/Lua/Python as the primary tier.
- **Hybrid** — Component/WIT plus host services and optional supervised native helpers.
- **Build** — centrally maintained cross-platform native build/signing service.
- **Remote** — browser/WebView or network service as the primary plugin execution model.

The scores evaluate an OpenCPN product architecture, not the intrinsic quality of each technology.

## Portability and language reach

| Criterion | CM | OOP | Script | Hybrid | Build | Remote |
|---|---:|---:|---:|---:|---:|---:|
| true OS/CPU portability | 5 | 1 | 4 | 5 for Wasm; 2 helpers | 1 | 4 |
| one unchanged package | 5 | 1 | 4 | 5 without helpers | 1 | 4 |
| C support | 3 | 5 | 2 | 4 | 5 | 2 |
| C++ support | 3 | 5 | 2 | 4 | 5 | 2 |
| Rust support | 5 | 5 | 2 | 5 | 5 | 3 |
| higher-level languages | 3 | 5 | 5 | 4 | 5 | 5 |
| Windows x86-64 | 5 | 4 | 5 | 5 | 4 | 5 |
| macOS Intel/ARM | 5 | 3 | 5 | 5 | 3 | 5 |
| Linux x86-64 | 5 | 4 | 5 | 5 | 4 | 5 |
| Linux ARM64/Raspberry Pi | 4 | 3 | 4 | 4 | 3 | 4 |
| Flatpak | 4 | 2 | 4 | 4 Wasm / 2 helper | 3 | 4 |
| status | P | F | F niche | **P, Prod-gated** | Prod complement | F niche |

Wasmtime's x86-64 Windows/macOS/Linux targets are Tier 1 and Linux/macOS AArch64 are Tier 2, not equal support claims ([support tiers](https://docs.wasmtime.dev/stability-tiers.html)). C/C++ components are possible but lack an integrated toolchain today ([C/C++ tooling](https://component-model.bytecodealliance.org/language-support/c.html)). JavaScript (`jco`) and Python (`componentize-py`) toolchains exist, but WASI 0.3 async support is still landing across guest languages. Rust is the credible first guest.

## Integration and execution

| Criterion | CM | OOP | Script | Hybrid | Build | Remote |
|---|---:|---:|---:|---:|---:|---:|
| OpenCPN integration effort | 3 | 2 | 3 | 2 | 4 | 2 |
| performance | 4 | 3 | 2–4 | 5 with host batches | 5 | 2 |
| startup latency | 3 | 2 | 3–5 | 3 | 5 | 2 |
| memory consumption | 3 | 2 | 2–4 | 3 | 5 | 2 |
| runtime/binary size | 2–3 | 3 | 2–5 | 2 | 5 core / 1 aggregate | 1–3 |
| large-data transfer | 3 | 2–4 | 3 | 5 | 5 | 2 |
| asynchronous operations | 4, new | 5 | 3–5 | 5 | 4 | 5 |
| threading | 3 | 5 | 2–4 | 4 | 5 | 3 |
| cancellation | 4 | 5 | 3 | 5 | 2 API-dependent | 4 |
| crash isolation | 3 | 5 | 2 | 4 | 1 current native | 4–5 |
| CPU/memory limits | 4 | 5 with OS controls | 2–3 | 5 | 1 | 4 server-side |

WASI 0.3 now has `stream`, `future` and async functions, but tooling is newly stabilising ([WASI 0.3](https://bytecodealliance.org/articles/WASI-0.3)). Wasmtime provides resource limiters, fuel/epoch interruption and async calls; the embedder must supply scheduling. OOP can use OS termination/cgroups/job objects, but high-volume IPC needs shared-memory ownership and validation. Cap'n Proto does not make zero-copy RPC automatic ([Cap'n Proto C++ RPC](https://capnproto.org/cxxrpc.html)).

Runtime-size scores are deliberately provisional. Wasmtime feature selection can reduce size, but stripping the compiler requires trusted host-generated precompiled artifacts; accepting guest-provided precompiled images is unsafe ([minimal embedding](https://docs.wasmtime.dev/examples-minimal.html), [precompilation warning](https://docs.wasmtime.dev/examples-pre-compiling-wasm.html)). Measure the linked OpenCPN artifact rather than quoting release archive sizes.

## Security and lifecycle

| Criterion | CM | OOP | Script | Hybrid | Build | Remote |
|---|---:|---:|---:|---:|---:|---:|
| default-deny capabilities | 5 | 4 | 2–4 | 5 | 1 current model | 4 |
| memory safety boundary | 5 | 5 process | 3 | 5/5 | 1 in-process native | 4 |
| runtime-defect containment | 2 | 5 | 2 | 3–5 by placement | 1 | 5 remote / 2 WebView |
| permission UX | 4 | 4 | 3 | 5 | 2 | 3 |
| deterministic force stop | 4 Wasm only | 5 | 2–4 | 5 helpers, 4 Wasm | 1 | 5 remote |
| package signing/update | 4 | 3 | 4 | 5 | 3 | 4 |
| dependency attack surface | 3 | 2 | 2–4 | 2–3 | 2 aggregate | 1–3 |
| malformed native parser containment | 2 if compiled in Wasm | 5 | 2 | 5 helper | 1 | 5 remote |

Wasm memory isolation is strong, but an embedded runtime and host imports share the OpenCPN process. Wasmtime advisories and monthly releases make a real security update process mandatory ([security advisories](https://github.com/bytecodealliance/wasmtime/security), [release/LTS policy](https://docs.wasmtime.dev/stability-release.html)). OOP is strongest for native parsers, but only if the helper receives narrow OS and protocol capabilities.

## Product integration and evolution

| Criterion | CM | OOP | Script | Hybrid | Build | Remote |
|---|---:|---:|---:|---:|---:|---:|
| host-rendered UI | 4 | 3 | 4 | 5 | 1 current API | 2 WebView / 3 remote |
| overlay rendering | 4 | 3 | 4 | 5 | 5 native | 2 |
| chart database access | 4 via service | 4 via RPC | 3 | 5 | 5 unsafe/native | 2 |
| debugging | 3 | 5 | 5 | 4 | 5 | 4 |
| developer tooling | 3 | 5 | 5 | 4 | 5 | 5 |
| API evolution | 5 WIT | 4 IDL | 3 | 5 | 1 current ABI | 4 |
| inter-plugin composition | 5 typed | 4 | 3 | 5 | 2 strings | 4 |
| package signing | 4 | 3 | 4 | 5 | 3 | 4 |
| dependency maintenance | 3 | 2 | 3 | 2 | 1 per matrix | 2 |
| licensing | 5 | 3–5 | 3–5 | 4 | 3 | 2–5 |
| maintainer burden | 3 | 2 | 3 | 2 initially | 2 | 1–2 |

Wasmtime is Apache-2.0; OpenCPN remains GPL-2.0. Each helper library and guest SDK still requires a licence/SBOM review. WIT's records, variants, results and resources are much better evolution material than C++ layouts. Major WIT interfaces must coexist and receive adapters; the Component Model alone does not supply OpenCPN's policy.

## Reference workload fit

| Workload property | CM | OOP | Script | Hybrid | Build | Remote |
|---|---:|---:|---:|---:|---:|---:|
| xGRIB viewer/timeline | 4 | 3 | 4 | 5 | 5 | 2 |
| large environmental sampling | 3 | 3 | 2–3 | 5 | 5 | 2 |
| ecCodes/NetCDF/PROJ generation | 2–3 unproven | 5 | 1 | 5 | 5 | 3 remote |
| malformed input isolation | 4 in Wasm | 5 | 2–3 | 5 helper | 1 | 5 remote |
| Weather Routing compute | 4 | 4 | 2–4 | 5 | 5 | 3 |
| batched chart safety | 4 | 3 | 3 | 5 host vector service | 5 native | 2 |
| route/corridor overlay updates | 4 | 3 | 3 | 5 | 5 | 2 |
| headless regression | 5 | 5 | 5 | 5 | 3 | 4 |

Pure Wasm becomes attractive for specialist libraries only if reproducible component builds, data bundles, performance, memory and malformed-input tests beat a helper's release burden. Until then the hybrid is honest about the exception.

## Candidate dispositions

### WebAssembly Component Model only — prototype-capable, incomplete production answer

Accept as the portable computation and interface foundation. Reject as the sole execution placement because native codec stacks, stronger crash isolation and Flatpak/helper realities remain. Do not expose generic WASI ambient authority.

### Out-of-process RPC only — reject as portable tier

Use for optional helpers and possibly a future high-isolation runtime mode. RPC is a transport, not a portable executable format. A single archive containing five native binaries is unchanged as an archive but has not eliminated the build matrix.

### Scripting runtime only — reject as primary tier

Useful as a guest authoring language or native compatibility plugin. Selecting JS/Lua/Python would still require the entire service/security/package design, while adding language-specific semantics and weaker resource isolation. Python native extensions recreate the architecture matrix.

### Hybrid — recommend

Best workload fit and honest fault/portability trade-off. More architecture work initially, but it prevents specialist libraries, chart internals and UI objects from contaminating the portable ABI.

### Central native build service — retain as complement

It reduces developer release engineering and builds helpers/native-only plugins. It does not provide build-once, does not remove native ABI failures and centralises signing/supply-chain burden.

### Remote service/WebView — reject as primary; defer restricted UI option

Remote compute can isolate expensive algorithms, but navigation must work offline, latency and credentials become network concerns, and server availability/cost replaces packaging. WebView is not an execution sandbox for privileged plugins and has accessibility, focus, bundle and renderer divergence costs.

## Final selection and maturity statement

Select **Hybrid** with Wasmtime Components/WIT as the only portable ABI. It is technically feasible and practical for the requested vertical slice. It becomes production-suitable only after platform, security-maintenance, guest-language, package-trust and performance gates pass. Shared memory, arbitrary WebView UI, raw sockets and portable native library claims are desirable in some cases but premature.
