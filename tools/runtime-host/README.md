# Isolated Linux development environment

`build-linux.sh` builds unmodified OpenCPN 5.14.0, builds the Portable Plugin
Manager independently, runs its unit tests, and installs both below
`/home/paul/RuntimeHost-OpenCPN` by default. Override that location with
`OCPN_RUNTIME_HOST_ROOT`.

`launch-linux.sh` uses a dedicated OpenCPN configuration directory, XDG roots,
native-plugin search path and portable-package store. It deliberately does not
change `HOME`. This protects the normal OpenCPN profile and the separate
Test-OpenCPN prototype.

`package-reference-plugins.sh` independently builds the native iGRIB helpers
and the current WIT-contract Wasm components for iGRIB, iWeatherRouting and
iPolars. It stages complete package roots and produces deterministic,
development-signed `.ocpnp` archives below
`/home/paul/RuntimeHost-OpenCPN/build-reference-packages/packages` by default.
This replaces the prototype's former dependency on modifying stock OpenCPN's
top-level CMake build.

Every package build runs archive-level signature, manifest, UI and helper
conformance. Set `PPM_CONFORMANCE_GRIB_FIXTURE` to a real GRIB file to add the
slower real decode and complete generator checks to that build.

The script generates a disposable ignored signing-key pair when needed and
places both `development-trust.pem` and a Package Store-compatible `trust/`
directory beside the packages. Development trust is test infrastructure, not
a production release credential.

Pass that key to a local Manager build as
`-DPPM_DEVELOPMENT_TRUST_KEY=/path/to/development-trust.pem`. This keeps
subsequent `cmake --install` operations aligned with packages signed by the
same disposable key. Omitting the option installs only the repository's
predefined development public keys.

`install-reference-packages.sh` refuses to run while the isolated OpenCPN is
active, checks that the installed development trust matches the archives, and
uses the Manager's own CLI/store implementation to inspect, transactionally
install or update, approve, enable and audit all three packages. `build-linux.sh`
runs both scripts, so a fresh isolated build reaches the same tested state
without manual file copying.

`measure-linux-resources.sh` temporarily preserves and varies only the
isolated package enable-state, then records two real OpenCPN launches for
stock, Manager-only, iGRIB, iGRIB+iWeatherRouting, and all-package cases. Its
TSV report includes installed bytes, startup time, settled RSS and startup
peak RSS. It restores the original package states even when interrupted and
refuses to run while the isolated application is open.

`measure-linux-workload.sh` complements that idle/startup report with two
complete installed-package Holyhead–Dún Laoghaire route runs against the real
44.9 MB GRIB fixture. It records elapsed time and maximum RSS, retains each
progress/result log, and requires reverse recovery plus independent validation
to appear in the result before accepting a run.

`measure-linux-igrib-workbench.sh` uses a developer-gated startup action to
open the real iGRIB workbench and load the real fixture in stock OpenCPN. It
records time to readiness, settled process-tree RSS and the peak including
supervised helpers, and rejects leaked helper processes.

## Renderer validation

The normal isolated 5.14 profile can validate the OpenGL callback by changing
only its copied test configuration to `OpenGL=1`, then starting iGRIB with:

```sh
OCPN_PPM_DEVELOPER_STARTUP_ACTION=org.opencpn.igrib:igrib.toggle \
OCPN_PPM_IGRIB_SMOKE_FIXTURE=/path/to/real.grb \
  tools/runtime-host/launch-linux.sh
```

The log must report a capable OpenGL context, `dataset-ready`, and:

```text
PPM event=environment-overlay-rendered path=opengl-compatibility dataset=ready
```

Vulkan validation requires an OpenCPN build which provides its experimental
Vulkan presenter (the frozen 5.14 baseline does not). Launch that executable
with a copied temporary configuration, the same `OPENCPN_PLUGIN_DIRS`,
`OCPN_PORTABLE_PLUGIN_ROOT`, developer action and fixture, plus
`--renderer=vulkan-experimental`. Acceptance requires an active Vulkan adapter,
`dataset-ready`, and:

```text
PPM event=environment-overlay-rendered path=wxdc dataset=ready renderer-compatible=software,vulkan
```

Never point this validation at the user's normal configuration directory. The
5.15 workstation test used a `/tmp` configuration and left the working
OpenCPN profile unchanged.

The checked-in desktop file is for this development workstation. Other
developers should change its absolute `Exec` and `Icon` paths after building.

## Headless package conformance

With `BUILD_TESTING` enabled, the host-plugin build also produces
`ppm_package_cli`. It exercises the same native verifier, transactional store
and permission records as the wx manager. For example:

```sh
cli=/home/paul/RuntimeHost-OpenCPN/build-ppm/ppm_package_cli
store=/home/paul/RuntimeHost-OpenCPN/config/portable-runtime
trust=/home/paul/RuntimeHost-OpenCPN/config/share/opencpn/plugins/portable_plugin_manager_pi/trust

"$cli" --root "$store" --trust "$trust" --developer inspect package.ocpnp
"$cli" --root "$store" --trust "$trust" --developer install package.ocpnp
"$cli" --root "$store" --trust "$trust" --developer audit org.example.package
"$cli" --root "$store" --trust "$trust" --developer permissions org.example.package
"$cli" --root "$store" --trust "$trust" --developer --yes approve org.example.package
"$cli" --root "$store" --trust "$trust" --developer state org.example.package on
```

`state ... on` fails unless both the installed-file audit and exact permission
approval pass. `--yes` is deliberately required for non-interactive approval.
The bundled public keys verify only this repository's disposable development
packages and are accepted only while developer mode is on; no private signing
key is installed or tracked.
