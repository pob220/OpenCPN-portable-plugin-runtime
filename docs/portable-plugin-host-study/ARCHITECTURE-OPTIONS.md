# Architecture options and decision matrix

## Decision

For the next demonstrable and broadly usable architecture, **Option 3 — one
standard managed runtime host plus a small generic plugin action API — is the
best choice (402/500)**. A stock-core Option 2 vertical slice precedes it and
already works. Before accepting an open-ended third-party package ecosystem,
evolve Option 3 toward **Option 7 — the same plugin adapter with Wasmtime and
high-risk services in an external broker**.

This is Outcome B, with a security-driven broker evolution. It is not a claim
that portable children become first-class managed OpenCPN plugins.

## Options

1. **Existing modified core.** Keep the supervisor, engine, UI hosts and
   ownerless contributions compiled into OpenCPN.
2. **Pure standard plugin.** Move everything possible behind API 1.21 and
   accept the proven toolbar workaround and reduced chart/identity semantics.
3. **Standard plugin plus generic API extensions.** Add stable dynamic toolbar
   actions (and later a bounded chart query only if required).
4. **Plugin plus thin core broker.** Keep privileged chart/action/lifecycle
   services in a small core service used by the plugin.
5. **External broker controlled by a pure plugin.** Keep current public API,
   put engine/packages/jobs in another process, and bridge actions/rendering.
6. **First-class core integration.** OpenCPN directly recognises, presents and
   governs each portable package.
7. **Hybrid plugin + generic actions + external runtime broker.** Discovered
   during the study: retain a small in-process public-API adapter and put
   Wasmtime, package execution and risky parsing outside the OpenCPN process.

Option 7 is not the first delivery because the current bridge is synchronous
and pointer/callback shaped; designing authenticated framed IPC, shared
immutable scene data and broker recovery before proving the host adapter would
front-load the largest uncertainty.

## Weighted matrix

Scores are 1 (poor) to 5 (strong). Weighted totals are out of 500. The weights
were chosen before totals were calculated and deliberately give security the
largest single weight.

| Criterion | Weight | O1 | O2 | O3 | O4 | O5 | O6 | O7 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Minimal core change | 10 | 1 | 5 | 4 | 3 | 5 | 1 | 4 |
| Functional preservation | 10 | 5 | 3 | 4 | 5 | 3 | 5 | 4 |
| Package icons/dynamic actions | 8 | 4 | 3 | 5 | 5 | 3 | 5 | 5 |
| User identity/experience | 7 | 3 | 3 | 3 | 4 | 3 | 5 | 3 |
| Isolation/security | 12 | 2 | 2 | 2 | 3 | 5 | 2 | 5 |
| Maintainability | 10 | 2 | 4 | 5 | 3 | 3 | 3 | 3 |
| Portability/packaging | 10 | 2 | 4 | 4 | 3 | 2 | 2 | 2 |
| Performance | 6 | 5 | 4 | 4 | 4 | 3 | 5 | 3 |
| Installation/catalogue fit | 6 | 2 | 5 | 5 | 4 | 3 | 5 | 4 |
| API stability/upstream review | 8 | 1 | 3 | 5 | 3 | 3 | 2 | 4 |
| Incremental migration | 7 | 5 | 4 | 5 | 3 | 3 | 2 | 3 |
| Governance/ecosystem risk | 6 | 3 | 2 | 3 | 3 | 3 | 5 | 3 |
| **Weighted total / 500** | **100** | **280** | **347** | **402** | **355** | **334** | **329** | **364** |

## Why the alternatives lose

- O1 preserves behaviour but makes every runtime security update an OpenCPN
  executable change, permanently couples large UI/service code to core and
  performs badly on upstream reviewability.
- O2 is the right technology-preview substrate. It loses beta points because
  late insertion needs an unrelated visibility setter, there is no native
  enabled state or child placement persistence, and chart semantics must be
  reduced.
- O4 is justified only if a privileged service is truly essential. No preview
  service met that test, and a generic public API is easier to review and reuse.
- O5 improves crash isolation but retains the toolbar deficits and adds IPC,
  process packaging and render latency before they are needed.
- O6 is the only way to create true child entries in OpenCPN's own manager, but
  it is the largest permanent core/security/governance commitment and does not
  itself isolate an embedded engine.
- O7 is the strongest open-ecosystem security design. It is a planned evolution,
  not the fastest way to establish behavioural parity.

## Sensitivity analysis

Three alternative, still plausible weight sets were tested:

| Priority set | O1 | O2 | O3 | O4 | O5 | O6 | O7 | Winner |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Security/governance first | 273 | 309 | 363 | 344 | 352 | 329 | **377** | O7 |
| User identity first | 288 | 334 | **391** | 365 | 319 | 385 | 355 | O3, O6 close |
| Avoid core change first | 247 | 375 | **404** | 348 | 360 | 288 | 371 | O3 |

The recommendation is stable for ordinary preview/beta priorities. It changes
to O7 when process isolation dominates. If separate standard Plugin Manager
entries become a hard acceptance condition rather than a weighted preference,
O6 wins by definition; toolbar identity cannot manufacture native package
identity.

## Recommended staged interpretation

- **Technology preview:** O2 on stock OpenCPN, desktop Linux first.
- **Broadly usable beta:** O3, desktop target matrix, host-managed child UX.
- **Third-party production ecosystem:** O7 security boundary while retaining
  the O3 OpenCPN adapter; pursue O6 only for first-class manager/governance UX.
- **Authoritative chart safety:** not part of any initial outcome. Add one
  generic immutable batch API only after semantics and consumers are agreed.
