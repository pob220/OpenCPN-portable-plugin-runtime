# Minimum OpenCPN changes

## Bottom line by maturity

| Maturity | Required OpenCPN change |
|---|---|
| Useful technology preview | **None.** Stock API 1.21 passed multiple icons, late add/remove, Wasmtime lifecycle, DC/GL overlays, job shutdown and navigation enumeration. |
| Broadly usable beta | **One generic stable dynamic toolbar-action API is recommended.** A secure-credential service is desirable if persistent secrets are a beta requirement. No runtime-specific core broker is required. |
| First-class production integration | Core/Plugin Manager/catalogue recognition of child packages is required only if each child must be a first-class managed OpenCPN plugin. A generic chart query is separately required only for authoritative chart/depth/hazard semantics. |

## Change 1 — stable dynamic plugin actions

### Problem and proof

API 1.21 has insert/remove/visibility/check/icon calls, so the preview works.
However:

- `PlugInManager::AddToolbarTool` assigns a process-local integer and does not
  request a rebuild;
- P1 had to call unrelated `SetToolbarToolViz(id, true)` after late insertion;
- there is no public native enabled state;
- `gui/src/toolbar.cpp::_toolbarConfigMenuUtil` skips plugin tools, so child
  visibility/placement are not customised or persisted;
- there is no stable plugin-owned action key or rebuild lifecycle contract.

These are general native-plugin limitations, not Wasmtime-specific needs.

### Proposed generic interface

The exact C++ ABI shape should follow current API-version conventions; the
semantic surface is:

```cpp
struct PluginToolbarActionSpec {
  std::string stable_key;       // unique within native plugin identity
  wxString label;
  wxString tooltip;
  PluginActionKind kind;        // command or check
  PluginIconSet icons;          // normal, rollover, toggled, optional disabled
  int order_hint;               // advisory
  bool visible_by_default;
};

struct PluginToolbarActionState {
  bool visible;
  bool enabled;
  bool checked;
};

PluginActionHandle RegisterToolbarAction(const PluginToolbarActionSpec&);
PluginActionResult UpdateToolbarAction(PluginActionHandle,
                                       const PluginToolbarActionState&);
PluginActionResult UpdateToolbarActionIcons(PluginActionHandle,
                                            const PluginIconSet&);
PluginActionResult UnregisterToolbarAction(PluginActionHandle);
```

Clicks are delivered through a new callback carrying the opaque handle and
stable key. Handles are valid until unregister/plugin unload and survive
toolbar UI rebuilds; they need not survive a process restart. Persistence uses
`(native managed-plugin identity, stable_key)`, never the integer/handle.

### Lifecycle, thread and error rules

- Register/update/unregister are UI-thread operations; an off-thread call
  returns `wrong_thread`, rather than implicitly marshalling and hiding order.
- Core validates non-empty bounded UTF-8 keys and rejects duplicates.
- Registration owns/copies all value fields and requests exactly one safe
  toolbar update internally.
- Unregister is idempotent and prevents future callbacks before returning.
- Plugin unload automatically unregisters remaining actions, but explicit
  cleanup remains supported.
- Update is atomic from the plugin's perspective and returns `not_found`,
  `wrong_owner`, `wrong_thread`, `invalid_spec` or `shutting_down`.
- `enabled=false` supplies native visual/accessibility semantics and suppresses
  callbacks in core.
- Core persists user visibility/order only for registered stable keys and
  retains bounded tombstones for temporarily absent actions.

Likely implementation sites are `include/ocpn_plugin.h`,
`gui/src/ocpn_plugin_gui.cpp`, plugin-manager action containers,
`gui/src/toolbar.cpp` and focused API/toolbar tests. It can be proposed and
reviewed independently of the portable runtime. Dynamic-source, mode and
device plugins all benefit.

### Tests

1. Two actions under one plugin and duplicate-key rejection.
2. Late add/remove/update without an explicit caller rebuild.
3. Native enabled/checked/visible semantics and callback suppression.
4. Stable key preference survival across rebuild, restart and temporary
   absence; handle invalidation across restart.
5. Plugin disable/unload while callbacks are queued.
6. Theme/DPI/touch rebuild and icon replacement.
7. Multiple canvases and toolbar customisation.
8. Wrong-thread/owner, shutdown and invalid-icon negative cases.

Fallback if rejected: keep P1's insert-plus-visibility workaround, own all
logical persistence in the host, hide/block initialising actions and document
the lack of native disabled/customised placement semantics.

## Change 2 — secure credential broker (conditional)

API 1.21 exposes configuration but no portable OS-keychain abstraction.
Persistent provider passwords should not be stored as normal package settings.
A generic service could store/retrieve/delete a credential under
`(native-plugin identity, plugin-defined account key)` with explicit user
consent and platform keychain backing. It must never return secrets to logs,
command lines or child processes except through an explicit scoped handoff.

This benefits all networked plugins and can be submitted independently.
Fallback: session-only memory plus user re-entry, which is acceptable for the
preview and a defensible beta compromise. It is not a reason for a runtime-
specific core broker.

## Change 3 — bounded chart information query (conditional)

### Current limitation

The current `charts.coverage` callback actually serialises calls to
`PlugIn_GSHHS_CrossesLand` and reports one chart considered. GSHHS is a coarse
coastline crossing helper, not chart coverage, depth or authoritative hazard
information. `GetChartDatabaseEntryXML` exposes metadata but not a thread-safe
bounded spatial query. Private chart objects/headers are not an acceptable
plugin dependency.

### Generic design constraints

Only pursue this after specifying a concrete, non-safety-misleading consumer.
A possible immutable batch request includes viewport/segment/point geometry,
requested information classes and result limits. Results carry source chart
ID/revision/scale, coverage status, conservative unknown states and explicit
advisory semantics. The API defines UI/worker-thread legality, snapshot
revision, maximum batch size, cancellation and `unavailable/unknown` rather
than fabricating safe water.

It should serve routing, survey, chart download/diagnostic and other native
plugins. Likely implementation crosses `ocpn_plugin.h`, chart database/query
code and source-specific adapters. Tests need chart fixtures, overlapping
scales, stale revisions, dateline/poles, unavailable charts and concurrent
queries.

Fallback: rename the portable service to advisory coastline crossing, retain
GSHHS, use a package-owned display land mask and exclude chart/depth safety
from the product. This fallback is recommended for the vertical slice.

## First-class child integration (not a minimal beta patch)

For separate standard manager entries, OpenCPN must define child package
identity, catalogue and signer policy, install/update/remove ownership,
permissions, crash attribution, translations, documentation and conflict
rules. That is a product/governance programme, not a toolbar API extension.
It can continue to use the native runtime host as its execution adapter, but
OpenCPN must become aware of each package.

No API should be named for iGRIB, iWeatherRouting, Wasmtime or this prototype.
