# iGRIB portable component

iGRIB is the standalone environmental reference plugin for OpenCPN's
experimental hybrid portable-plugin runtime. It does not load, call or require
the native xGRIB plugin. The portable Component Model guest registers actions,
uses typed OpenCPN services and requests a host-owned environmental surface.

The current proof implements:

- an accessible, host-rendered xGRIB-style viewer with timeline playback and
  wind, current, pressure, wave-height and air-temperature layers;
- ecCodes metadata and frame decoding in a separately supervised helper;
- GFS/UKMO environmental generation plus optional waves and authenticated
  Copernicus Marine North-West Shelf or global current
  inputs through the signed `environmental-grib` helper;
- user-selected input/output paths, namespaced private storage and a
  permission-controlled host HTTP client;
- retained geographic overlays, batched chart-coverage queries, settings,
  cancellable jobs and deliberate-trap containment;
- deterministic `.ocpnp` packaging, Ed25519 developer signing, strict archive
  verification, atomic update and retained rollback copies.

On Linux the helper is launched with explicit read-only/input/output mounts in
a bubblewrap user/PID/IPC/UTS namespace and CPU/address-space limits. OpenCPN
owns every wxWidgets and chart/rendering object; neither the component nor its
helpers receive a graphics context or OpenCPN pointer.

This remains experimental. The included signing key is deliberately public
development material and is not a production trust root. Linux x86-64 has
real-file, real-network and GUI evidence; Windows, macOS, Linux ARM64,
Raspberry Pi and Flatpak must run `portable-runtime/tests/conformance.py` with
target-built helper payloads before being described as supported. Generated
weather and plugin overlays are planning aids, not authoritative navigation
products.
