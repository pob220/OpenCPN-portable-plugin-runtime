# Third-party plugin portability audit

Status date: 2026-07-24. This is a source-level capability audit, not a claim
that the native C++ plugins run unchanged. Portable plugins are Component
Model applications and must be rewritten against the versioned WIT API.

The inspected public revisions were:

| Plugin | Revision | Main native dependencies |
|---|---|---|
| [testplugin_pi](https://github.com/jongough/testplugin_pi) | `dfde2b8` (2026-06-19) | toolbar, modeless dialog, cursor/mouse/key input, JSON plugin messaging |
| [ocpn_draw_pi](https://github.com/jongough/ocpn_draw_pi) | `82b3adb` (2025-11-12) | multiple actions, context menu, large wx UI, persistent objects, viewport/input, wxDC/OpenGL overlays, plugin messaging |
| [crowdsource_pi](https://github.com/jongough/crowdsource_pi) | `9dba176` (2025-01-15) | NMEA/position, preferences, SQLite queue, threaded raw socket/Avro transport |
| [windvane_pi](https://github.com/jongough/windvane_pi) | `a49c24b` (2023-06-28) | toolbar/preferences, position/NMEA, NMEA output, viewport overlay, keyboard and plugin messaging |

## API mapping

| Native pattern | Portable API `0.3` replacement |
|---|---|
| `InsertPlugInTool*`, context menu callbacks | typed `actions` registration and state |
| wxWidgets manager/preferences dialogs | manifest-declared, host-owned declarative `surfaces` |
| config object and private files | `settings` and atomic `private-storage` |
| file open/save controls | host picker plus single-use `user-files` grant |
| position, NMEA, Signal K, AIS, active leg | filtered capability `event` subscriptions |
| `PushNMEABuffer` | validated, rate-limited `navigation-output.send-nmea0183` |
| wxDC/OpenGL overlay callbacks | retained `scenes`; host renders for software/Vulkan and OpenGL |
| cursor/mouse/key callbacks | bounded pointer/key sink; interactive scene IDs are package scoped |
| route/waypoint/track APIs | bounded typed reads and user-confirmed mutations |
| ad-hoc JSON ODAPI exchange | versioned asynchronous `plugin-rpc` |
| `wxTimer` | lifecycle-owned `timers` |
| native plugin messages | bounded compatibility `plugin-messages` plus receive event |

## Plugin conclusions

### testplugin_pi

The host capabilities are sufficient for a clean rewrite. The useful result
would not be a line-for-line emulation: its JSON ODAPI tests should become an
API `0.3` conformance client for typed RPC, actions, input and declarative
surface events.

### windvane_pi

The core behavior is representable. NMEA input/output, position, keyboard,
toolbar state, private settings, host-owned UI and renderer-independent
overlays are present. The rewrite must parse and validate its own navigation
sentences and should use typed RPC where another portable package offers a
service.

### ocpn_draw_pi

The architecture can support a functional rewrite, but this is the largest
port:

- drawing objects become stable-ID scene primitives and private value data;
- manager/property dialogs become declarative surface documents and state;
- pointer events provide geographic coordinates and a coarse host hit hint;
  exact segment/object selection remains deterministic guest geometry;
- calls intended for another plugin become typed RPC; and
- any intentional OpenCPN waypoint/route/track change crosses the user
  confirmation boundary.

The portable host does not expose wxWidgets objects, graphics contexts,
OpenGL/Vulkan handles, OpenCPN private classes or raw pointers. Those
omissions are design properties, not missing compatibility shims.

### crowdsource_pi

This is the known blocker. Private storage can replace its local SQLite queue
and the event broker supplies its navigation data, but its native threaded
socket/Avro protocol has no portable equivalent. Raw sockets should not be
added. A future host service must instead provide bounded cancellable HTTPS
requests/uploads with an explicit redirect, header, credential and byte
policy. The server protocol may also need an HTTPS ingestion endpoint.

## Decision

API `0.3` is ready for third-party experimentation with local, interactive
and navigation-data plugins. It is not yet complete for network-server
plugins. That distinction should be retained in release notes and catalogue
decisions.
