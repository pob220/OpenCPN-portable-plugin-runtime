# Native Demo Plugin to OPP API comparison

Status date: 2026-07-27. The comparison baseline is the documented OpenCPN
[Demo Plugin](https://opencpn-manuals.github.io/main/demo_plugin/index.html).
It is a coverage checklist, not a claim that native C++ plugins can be
mechanically recompiled as portable components.

| Demo capability | OPP API 0.4 mapping | Status / boundary |
|---|---|---|
| identity, version, description | signed manifest plus lifecycle `plugin-info` | covered |
| logging and user units | diagnostics plus `host-environment` | covered; no host filesystem paths |
| settings/defaults | package-isolated settings and private storage | covered |
| preferences/options | declarative surface roles | covered as host-owned frames; core options-notebook embedding is deferred |
| toolbar and root context actions | typed action registration/invocation | covered |
| AIS/route/waypoint/track submenus | extended action locations and object context | covered |
| position, COG/SOG, headings, variation, fix time | typed vessel position and position events | covered |
| filtered NMEA 0183, NMEA 2000, Signal K | typed bounded subscriptions | covered |
| NMEA 0183 transmit | validated and rate-limited host output | covered |
| NMEA 2000 transmit | opaque output IDs, registration and informational-PGN allowlist | deliberately narrower than native |
| OpenCPN messages | bounded plugin-message events/send and typed portable RPC | covered; typed RPC preferred |
| waypoint/route/track reads and writes | paged objects, revision tokens and confirmed mutation | covered; every mutation remains user-confirmed |
| canvas drawing and input | retained multi-canvas scenes and scoped pointer/key events | covered without renderer access |
| colour scheme/DPI | host-environment snapshots and events | covered |
| timers/background work | lifecycle-owned timers, serial executor and bounded jobs | covered |
| Internet access | controlled HTTPS broker | covered for exact manifest domains; no raw sockets |
| dockable wxAUI dashboard | `dockable-panel` semantic role | partial until public OpenCPN docking API exists |
| native chart class, native wxWidgets/OpenGL, arbitrary libraries | none | intentionally outside OPP |

## Resulting design rule

The native Demo Plugin remains the compatibility inventory for public OpenCPN
features. OPP should add typed value contracts for generally useful
capabilities, but it must not copy native handles or GUI/rendering object
models into WIT. If a safe host-owned mapping does not exist, the matrix marks
the feature partial or out of scope rather than weakening isolation.
