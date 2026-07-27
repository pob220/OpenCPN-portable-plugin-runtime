# OPP API 0.4 author guide

OPP API 0.4 is the current general author profile. Its WIT namespace is
`opencpn:opp@0.4.0`; a package selects the exact compatible profile with:

```json
{
  "runtime": ">=0.1.0 <0.2.0",
  "portable_api": ">=0.4.0 <0.5.0",
  "portable_world": "plugin"
}
```

The WIT files under `contracts/0.4` are authoritative. This guide explains the
host policy around them.

## Capability model

Every host operation is checked against the package's signed permission list.
Guests receive values and opaque package-scoped identifiers, never native
pointers, wxWidgets objects, chart objects, renderer handles, driver handles
or sockets. Disable, failure and unload revoke actions, surfaces, events,
timers, scenes and RPC registrations owned by that package generation.

The main 0.4 additions are:

- typed event payload variants, including enriched vessel position and host
  environment snapshots;
- action invocation context for toolbar, chart, AIS, route, waypoint and track
  locations;
- semantic surface roles for preferences, tools, tasks, inspectors and
  dockable panels;
- cursor-paged navigation values with stable revisions and confirmed writes;
- retained scenes with explicit canvas targets, render phase, dash patterns,
  icon anchors/rotation and text placement;
- opaque communication-output endpoints and restricted informational NMEA
  2000 transmission; and
- bounded host-brokered HTTPS.

## Manifest additions

`ui.surfaces` is required to open a declared host-owned surface.
`communications.outputs.read` lists only package-scoped endpoint IDs.
`navigation.nmea2000.write` permits the host's informational-PGN allowlist;
the host still validates endpoint, destination, priority, payload and rate.

HTTPS requires both `network.https` and an `https_domains` array:

```json
{
  "permissions": ["network.https"],
  "https_domains": ["api.weather.example"]
}
```

Domains are lower-case exact DNS names. Wildcards, ports, URL fragments and IP
literals are invalid. Requests are HTTPS-only with certificate verification,
bounded methods, headers, request/response sizes, timeouts and redirects.
Every redirect is checked against the same exact allowlist; private,
loopback, link-local and multicast destinations are rejected after DNS
resolution. Raw sockets are not part of OPP API.

## Compatibility

API 0.1, 0.2 and 0.3 remain frozen side-by-side contracts. A 0.3 component is
not silently treated as 0.4. Migrate by rebinding against `contracts/0.4`,
changing imports from `opencpn:portable` to `opencpn:opp`, selecting the
`plugin` world, handling typed events and accepting `action-invocation` in the
lifecycle callback.

Surface roles describe intent, not access to native GUI objects. With the
current public OpenCPN plugin boundary, preference and dockable roles use
host-owned frames. True options-notebook or wxAUI embedding can be added later
without changing portable UI documents if OpenCPN exposes a stable public API.
