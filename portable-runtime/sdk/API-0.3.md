# Portable plugin API 0.3 author guide

API `0.3` is the general third-party author profile hosted by the conventional
Portable Plugin Manager. A package is a signed manifest, a WebAssembly
Component Model component, declared UI/resources and a complete checksum
inventory. OpenCPN loads only the native Manager; the Manager verifies and
owns portable package lifecycle.

## Design boundary

Guests exchange bounded values through WIT. They never receive native
pointers, wxWidgets objects, OpenGL/Vulkan handles, OpenCPN private classes,
arbitrary filesystem access or raw sockets. This makes one component usable
across host platforms while keeping policy and renderer integration native.

API `0.1`, `0.2` and `0.3` bindings coexist. A package manifest selects one
range and world; the host does not silently reinterpret older packages.

## Host imports

| Interface | Purpose | Principal permission |
|---|---|---|
| `diagnostics` | bounded host log records | none |
| `settings` | small package-private string settings | `settings.read-write` |
| `actions` | toolbar/context actions and state | `ui.commands` |
| `surfaces` | open a manifest-declared host UI | `ui.commands` |
| `navigation` | vessel position and bounded navigation objects | matching navigation read/write permission |
| `scenes` | retained host-rendered geographic primitives | `overlay.submit` |
| `private-storage` | atomic package-private values | `storage.private` |
| `user-files` | one operation through a host picker grant | `storage.user-selected` |
| `timers` | lifecycle-owned one-shot/repeating timer | `timers.schedule` |
| `navigation-output` | validated NMEA 0183 output | `navigation.nmea.write` |
| `plugin-messages` | compatibility output to native OpenCPN plugins | `plugin.messages.send` |
| `plugin-rpc` | async typed portable-package services | `plugin.rpc.request` / `plugin.rpc.provide` |
| `event-subscriptions` | lifecycle-owned filtered live events | event-specific read permission |

Guest exports receive lifecycle, declarative-surface, event, pointer/key,
timer and RPC callbacks. Callbacks for one package are serialized and tagged
with its current lifecycle generation.

## Important limits

- Scenes: 128 layers, 4,096 primitives and 250,000 polyline/polygon points per
  final scene. Revisions must increase. Incremental merges are checked against
  the same final totals.
- Icons: package-relative regular files, at most 512 by 512 display pixels.
- Timers: 32 per package, 50 ms to 24 hours.
- RPC: 64 services and 128 outstanding requests per package, 1 MiB payload,
  100 ms to 60 s timeout. Dispatch is asynchronous.
- Event subscriptions: at most 64 manifest entries, queue limit 1..1,024,
  topic prefix at most 256 UTF-8 bytes.
- Private/user file operations: 8 MiB per value in the current host policy.
- Navigation writes: every mutation requires host-side user confirmation.
- NMEA output: one valid bounded sentence; host checksum and rate policy
  applies.

Treat these as enforced resource policies, not recommended targets. Keep
normal updates much smaller.

## Rendering and interaction

A scene contains stable layer and primitive IDs. The host renders the same
polyline, polygon, circle, icon and text values through wxDC/software/Vulkan
or the bounded OpenGL compatibility path. Guests must not branch on the host
renderer.

Marking a primitive interactive gives pointer callbacks its scene and
primitive IDs when the host's bounded coarse hit region matches. This is a
hint. Plugins needing exact segment, polygon or handle selection must perform
their own geographic geometry test.

## UI and files

Declare each surface in `manifest.json` and include its UI document as a
resource. `surfaces.open` asks the host to present it. The
`surface-event-sink` returns the next bounded state document.

File controls return short-lived opaque grant tokens, not paths. Use
`user-files.read` or `user-files.write` once for the selected operation.
Persistent internal data belongs in `private-storage`; user preferences may
use `settings`.

## Events and interoperability

Prefer typed RPC for new portable-to-portable protocols. Register a stable
service name, document its methods/content types, use unique correlation IDs
and handle timeout/error responses. Do not synchronously wait inside a guest
callback: responses arrive later through `rpc-sink`.

`plugin-messages.send` and the plugin-message event remain for interoperability
with native OpenCPN plugins. They are bounded compatibility channels, not a
substitute for a versioned RPC contract.

The event broker can provide NMEA 0183, common NMEA 2000 PGNs, Signal K,
position, AIS targets, active-leg, cursor, viewport and native plugin
messages. Declare the narrowest topic prefix and smallest practical queue.

## Network status

API `0.3` intentionally has no raw sockets. The current general author profile
also does not yet expose a request/upload interface. Network-server plugins
must wait for the planned bounded asynchronous HTTPS capability; do not work
around this with WASI socket access or a bundled unsupervised helper.

## Development workflow

```sh
python3 portable-runtime/sdk/tools/portable_plugin.py new \
  --id org.example.my-plugin --name "My plugin" /tmp/my-plugin
python3 portable-runtime/sdk/tools/portable_plugin.py build \
  /tmp/my-plugin/Cargo.toml
python3 portable-runtime/sdk/tools/portable_plugin.py lint \
  /tmp/my-plugin/package/manifest.json
python3 portable-runtime/sdk/tools/portable_plugin.py package \
  --manifest /tmp/my-plugin/package/manifest.json \
  --component /tmp/my-plugin/target/wasm32-wasip2/release/portable_plugin_template.wasm \
  --output /tmp/my-plugin.ocpnp
```

Run the generated component against `ppm_portable_fake_host`, then install the
development archive only into an isolated OpenCPN profile. Production
catalogue packages require the separate publisher-signing and cross-target
conformance workflow.
