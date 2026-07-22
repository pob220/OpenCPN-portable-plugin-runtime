# Executive conclusion

## Primary recommendation

Adopt **Outcome B**: make the portable runtime one conventional managed
OpenCPN host plugin, deliver the first technology preview against completely
stock OpenCPN API 1.21, and pursue one small generic stable dynamic toolbar-
action API for beta quality. Keep the existing in-process Wasmtime bridge for
project-signed preview packages, but design the adapter so component execution,
package parsing and risky helpers can move to a supervised broker before an
open third-party production ecosystem is enabled. Do not retain the modified-
core architecture merely for lifecycle, toolbar, overlays, jobs, storage,
networking or navigation: experiments found public-plugin routes for them.

Confidence: **high** for the architecture and toolbar result on Linux x86-64;
**medium** for a broadly distributed desktop beta pending executed target
packaging/conformance; **unsupported/deferred** for Android.

## Decisive conclusions

- A useful version can run on stock OpenCPN with **no core changes**.
- One native host can expose separate manager, iGRIB and iWeatherRouting icons.
- Those icons were added and removed after initialisation without restarting
  OpenCPN and clicks can map through `(package_id, action_id)` to transient IDs.
- Independent check state works. Host-level enable/dispatch works, but API 1.21
  has no native per-tool disabled state.
- Late insertion currently needs `SetToolbarToolViz(new_id, true)` solely to
  force the rebuild which insertion omits. That is acceptable for a preview,
  not the preferred beta contract.
- Children remain portable packages/child extensions owned by the host. They
  are not separate standard Plugin Manager entries.
- The exact current chart service is advisory GSHHS coastline crossing, not
  chart coverage/depth/hazard safety. Reduce/rename it for the preview.
- In-process Wasm traps are contained; native host/engine aborts are not.

## Five strongest pieces of evidence

1. The stock-5.14 public-API probe registered three distinct SVG actions,
   changed state, dynamically removed/reloaded them after trap/disable, rejected
   collisions and left no action at `DeInit`.
2. The existing clean Rust bridge linked into that ordinary plugin, loaded the
   existing iGRIB component, completed lifecycle/action calls and shut down
   cleanly. Standalone bridge smoke also passed both existing components.
3. The guest's five-point retained scene rendered through both public software
   and OpenGL multi-canvas callbacks on the unmodified executable.
4. A guest-requested worker stayed off the UI path and cancelled/joined in
   30 ms during plugin shutdown; Linux helper process-group cleanup also passed.
5. Core diff inventory found the permanent coupling concentrated in lifecycle,
   ownerless toolbar dispatch, render forwarding, globals and private basemap
   classification; all but stronger chart semantics have public/plugin-owned
   replacements.

## Required OpenCPN changes by maturity

| Stage | Core/API need |
|---|---|
| Technology preview | None |
| Broadly usable beta | Generic stable dynamic toolbar actions recommended; secure credential API optional depending on persistent-secret requirement |
| First-class production child identity | Core/Plugin Manager/catalogue recognition and governance required |
| Authoritative chart/depth/hazard use | Separate bounded generic chart-information API required; not needed for the proposed slice |

No thin runtime-specific core broker is recommended.

## Principal compromises

- only the host appears in standard Plugin Manager;
- stock toolbar customisation does not persist child placement/visibility and
  cannot natively grey an action;
- package update/signing creates a separately governed child ecosystem;
- Wasmtime plus native helpers materially increases package size and security-
  update responsibility;
- native-helper containment differs by platform;
- current evidence executes Linux x86-64 only;
- Android is not part of the supported claim;
- project-signed in-process preview is not equivalent to a production sandbox.

## Recommended next action

Implement the smallest vertical slice on stock OpenCPN: a signed reduced
iWeatherRouting package loaded by the native host, with its own toolbar icon,
declarative window, public navigation snapshot, retained software/GL overlay,
cancellable job and persisted setting; disabling the package must retract all
contributions without restarting OpenCPN. This establishes the architecture
before GRIB helper/network packaging and before any upstream API proposal.

The implementation order, acceptance gates and rollback paths are in
`MIGRATION-PLAN.md`; the full evidence and decision are in
`FEASIBILITY-STUDY.md`.
