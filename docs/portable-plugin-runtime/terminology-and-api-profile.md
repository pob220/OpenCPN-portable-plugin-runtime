# OPP terminology and API profile

Status date: 2026-07-27.

## Stable names

Use these names in code, manifests and documentation:

| Name | Meaning |
|---|---|
| **OpenCPN Portable Plugin API (OPP API)** | The versioned, capability-limited WIT contract presented to a portable plugin. |
| **OpenCPN Portable Plugin Runtime** | The complete architecture: package format, trust policy, Manager, Wasmtime bridge, host services, SDK and conformance suite. |
| **Portable Plugin Manager** | The conventional native OpenCPN host plugin which implements the OPP API. It is not the API itself. |
| **OpenCPN Portable Plugin package** | The deterministic signed `.ocpnp` archive installed by the Manager. |

“Portable API” remains acceptable only when discussing historical API
0.1–0.3 material. New public material should use **OPP API** and include its
version.

## Version and profile policy

The WIT package identifier for the first named profile is
`opencpn:opp@0.4.0`; manifests select it with
`">=0.4.0 <0.5.0"` and world `"plugin"`.

API 0.1, 0.2 and 0.3 are frozen compatibility contracts. The Manager loads
them side by side and does not reinterpret their payloads. OPP API 0.4 is an
additive successor, not an in-place mutation:

- typed event variants replace the old kind-plus-JSON envelope;
- action callbacks include toolbar/chart/AIS/route/waypoint/track context;
- the host environment exposes locale, colour scheme, display scale and user
  units;
- navigation objects include visibility, active state, stable revision tokens
  and cursor-based pages;
- scenes select canvases and render phase and include richer placement/style;
- output endpoints are package-scoped opaque IDs;
- NMEA 2000 output is restricted to informational PGNs; and
- HTTPS is brokered by the host, bounded and restricted to exact domains
  declared in `https_domains`.

The package format, runtime version and OPP API version remain independent.
This prevents a package-container change from silently changing guest
semantics.

## Host integration profile

The Portable Plugin Manager uses only the public `ocpn_plugin.h` boundary.
Consequently:

- `preferences` and `options-page` are host-owned preference-style surfaces;
- `tool-window`, `inspector` and `dockable-panel` use host-owned tool frames;
- a true OpenCPN options-notebook page or wxAUI pane is not promised until a
  stable public core API exists; and
- OPP does not expose native wxWidgets windows, renderer objects, chart
  objects, driver handles or sockets to guests.

The role distinction is still contractual: it lets a future Manager map the
same portable UI document into stronger native placement without changing the
guest.
