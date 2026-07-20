# iGRIB portable component

iGRIB is the standalone environmental reference plugin for OpenCPN's
experimental hybrid portable-plugin runtime. It does not load, call or require
the native xGRIB plugin. The portable Component Model guest registers actions,
uses typed OpenCPN services and requests a host-owned environmental surface.

The current proof implements:

- an accessible, host-rendered xGRIB-style viewer with timeline playback,
  chart-cursor values and wind, current, pressure, wave-height and
  air-temperature layers;
- persistent display settings for wind barbs/arrows, vector and scalar
  density, overlay opacity, playback interval and timeline looping;
- ecCodes metadata and frame decoding in a separately supervised helper;
- GFS/UKMO environmental generation plus optional waves and authenticated
  Copernicus Marine North-West Shelf or global current
  inputs through the signed `environmental-grib` helper;
- explicit **Local GRIB file…** weather and current source choices whose
  selected inputs replace the corresponding online source and are validated
  and merged into one output;
  local weather files retain any wave records they contain without triggering
  an unexpected online wave download;
- user-selected input/output paths, namespaced private storage and a
  permission-controlled host HTTP client;
- retained geographic overlays, batched chart-coverage queries, settings,
  cancellable jobs and deliberate-trap containment;
- package-owned weather, fault-containment and HTTP-download toolbar icons,
  decoded by the host without exposing a native toolbar or graphics object;
- deterministic `.ocpnp` packaging, Ed25519 developer signing, strict archive
  verification, atomic update and retained rollback copies.

On Linux the helper is launched with explicit read-only input-file and output
directory mounts in a bubblewrap user/PID/IPC/UTS namespace and with
CPU/address-space limits. Selecting a local GRIB does not grant access to its
parent directory. OpenCPN owns every wxWidgets and chart/rendering object;
neither the component nor its helpers receive a graphics context or OpenCPN
pointer.

This remains experimental. The included signing key is deliberately public
development material and is not a production trust root. Linux x86-64 has
real-file, real-network and GUI evidence; Windows, macOS, Linux ARM64,
Raspberry Pi and Flatpak must run `portable-runtime/tests/conformance.py` with
target-built helper payloads before being described as supported. Generated
weather and plugin overlays are planning aids, not authoritative navigation
products.
