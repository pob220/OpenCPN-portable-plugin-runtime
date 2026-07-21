# Linux beta testing: portable runtime, iGRIB and iWeatherRouting

This branch is a source-level beta kit for OpenCPN 5.14.0 with the experimental
portable-plugin runtime plus the iGRIB provider and iWeatherRouting consumer
demonstration plugins. The build and launcher deliberately use an isolated
directory. They do not install into
`/usr`, do not use a normal `~/.opencpn` profile and do not require removal of
another OpenCPN version.

The locally demonstrated target is 64-bit Linux on x86-64. The repository also
contains a required seven-target CI matrix for Windows x86-64, macOS Intel and
Apple Silicon, Linux x86-64/ARM64, and Flatpak x86-64/ARM64. A target is a
supported beta target only when its current workflow job is green; a workflow
definition is not evidence by itself. Do not use
this experimental build as a primary navigation system. Its weather, overlays
and chart-coverage observations are not authoritative navigation products.
See the evidence rules and current rows in
[conformance-matrix.md](docs/portable-plugin-runtime/conformance-matrix.md).

## What the beta kit contains

- OpenCPN 5.14.0 source with the portable runtime behind
  `OCPN_ENABLE_PORTABLE_PLUGINS`;
- Wasmtime Component Model host and typed WIT interfaces;
- the iGRIB WebAssembly component, declarative UI and manifest;
- the iWeatherRouting WebAssembly component, declarative UI and manifest;
- typed, batched environmental exchange between the two portable plugins;
- standalone ecCodes decoding and environmental-generation helpers;
- the exact tested generator source revision as a Git submodule;
- deterministic developer-signed `.ocpnp` packaging and verification;
- isolated build, install, launch, conformance and diagnostic tools.

The checked-in Ed25519 key is deliberately public development material. It
tests the signature path but is not a production publisher identity.

## 1. Clone all source

```sh
git clone --branch portable-plugin-runtime --recurse-submodules \
  https://github.com/pob220/OpenCPN-portable-plugin-runtime.git
cd OpenCPN-portable-plugin-runtime
```

If the repository was cloned without submodules:

```sh
git submodule update --init --recursive
```

Confirm that `portable-runtime/vendor/environmental-grib-generator` is not
empty before building.

## 2. Install Linux prerequisites

The convenient package list targets Debian 12 and Ubuntu 24.04 or later. It
prints its actions by default; add `--install` only after reviewing them:

```sh
portable-runtime/beta/debian-prerequisites.sh
portable-runtime/beta/debian-prerequisites.sh --install
```

Other distributions need equivalent OpenCPN, ecCodes, NetCDF, PROJ, JsonCpp,
curl, Qhull, Blosc and libzip development packages. Runtime containment also
requires `bwrap` and `prlimit`. On Arch Linux the additional generator packages
are `eccodes jsoncpp netcdf curl qhull bzip2 blosc libzip proj bubblewrap
util-linux`.

Use an up-to-date stable Rust installed through the official
[rustup instructions](https://rust-lang.org/install.html). Wasmtime 46 requires
Rust 1.94 or later. After installing rustup, start a new shell and verify:

```sh
rustup toolchain install stable
rustup default stable
rustc --version
cargo --version
```

Python must be able to import `cryptography`; the Debian/Ubuntu script installs
`python3-cryptography`.

## 3. Build, test and stage everything

From the repository root:

```sh
portable-runtime/beta/build-linux.sh
```

The script performs the complete reproducible workflow:

1. validates the host and submodule;
2. installs the `wasm32-wasip2` Rust target and fetches locked crates;
3. builds and tests the pinned environmental generator;
4. configures OpenCPN with the experimental feature and test flags;
5. builds OpenCPN, the Wasmtime bridge, both components and iGRIB's helpers;
6. runs the portable CTest suite;
7. installs OpenCPN below `build-portable-beta/stage/app`;
8. verifies and installs both signed packages into the isolated profile;
9. writes a build-identity record.

Nothing is installed system-wide and `sudo` is not used by the build script.
The first build downloads Rust crates and can take a substantial amount of
time and disk space. Later builds use the locked Cargo cache and are
incremental.

Useful overrides are environment variables:

```sh
OCPN_BETA_JOBS=8 \
OCPN_BETA_BUILD_ROOT=/fast-disk/opencpn-portable-build \
OCPN_BETA_STAGE_ROOT=/fast-disk/Test-OpenCPN \
  portable-runtime/beta/build-linux.sh
```

Set `OCPN_BETA_SKIP_FETCH=1` only after all Cargo dependencies and the Wasm
target are present locally. Advanced testers can override the individual
OpenCPN or generator build directories; run the script with `--help` for the
complete list.

## 4. Launch the isolated build

```sh
portable-runtime/beta/launch-linux.sh
```

The launcher sets isolated `HOME`, XDG and plugin paths, enables software
rendering for the first run and passes an explicit configuration directory.
It does not inherit test hooks or GPU-selection variables from the calling
shell. To pass an OpenCPN option, append it after `--`, for example:

```sh
portable-runtime/beta/launch-linux.sh -- --fullscreen
```

iGRIB should load and open automatically. iWeatherRouting has its own route
toolbar icon. A separate iGRIB toolbar action remains
available if the window is closed. The three portable actions use distinct
icons: the blue **i** and wave icon opens iGRIB, the warning icon deliberately
tests Wasm trap containment, and the download icon tests the permission-gated
host HTTP client. The latter two are diagnostic actions, not normal forecast
controls.

## 5. Suggested beta exercise

Use non-critical sample data and record each result:

1. Confirm the title is **iGRIB — Portable Environmental Data**.
2. Open a valid GRIB and verify its forecast times appear. Open the picker
   again and verify it returns to the directory containing the successfully
   loaded GRIB, rather than GTK's unrelated global file-chooser directory.
   Generated GRIBs should likewise become the remembered location.
3. Toggle all fields present in the file. The schema covers all thirteen xGRIB
   groups: wind, gust, pressure, waves, current, precipitation, cloud, air
   temperature, sea temperature, CAPE, composite reflectivity, geopotential
   height and relative humidity. Select surface or the available 850, 700, 500
   and 300 hPa levels. Move the chart cursor and verify that decoded values are
   shown in the iGRIB window.
   Pan across a coastline and verify that current and wave symbols, scalar
   maps, numeric labels and cursor readouts stop at land while wind, pressure
   and air temperature remain available over land. The mask uses the bundled
   shapefile basemap and is a display aid, not a chart-safety assertion.
   Wave readouts include significant height, peak period and direction when
   these fields are present. Some products publish waves every three hours
   among hourly weather/current records; on an intervening timeline step,
   verify that waves remain visible and iGRIB names the wave source time in its
   status line.
4. Open **Settings**. Every declared field has a settings page. Change units,
   each field's display colour, vectors, colour overlay/palette, values,
   fixed-grid versus minimum-distance placement, spacing, contours and contour
   labels where applicable. Verify
   proper meteorological wind barbs and the single/double/tidal-style
   proportional current arrow forms. Exercise all three wave-specific forms:
   crest-and-travel markers, travel-direction arrows and height circles with a
   direction tick. Change wave colour, symbol size and spacing and verify that
   the complete symbol—not just its numeric label—changes colour. For
   proportional current arrows, adjust both the baseline size and
   pixels-per-knot growth; verify that weaker currents remain smaller than
   stronger currents and that spacing remains independently adjustable.
   Currents default to arrows only; the optional current colour overlay can be
   enabled deliberately if a magnitude map is wanted. Also change opacity,
   playback speed and timeline looping, then verify that all choices survive
   closing and reopening iGRIB.
5. Step forward/backward, drag the time slider, run timeline playback, switch
   pressure levels, enable interpolation, and open the cursor weather table.
   Open multiple GRIB files in a deliberate order and verify deterministic
   last-file-wins handling for duplicate messages.
6. Open **Generate GRIB** and inspect each selector. Weather choices include
   GFS, HRRR, UKV, ICON-EU, ECMWF IFS/AIFS and a local file. Waves include GFS
   Wave and Copernicus global waves. Currents include automatic regional
   selection, Marine.ie, Copernicus NWS/global/IBI/Mediterranean, RTOFS, local
   GRIB/NetCDF, TPXO cache/direct and offline tidal sources. Each package-owned
   entry states its account/file/model requirement, coverage and limitation.
   For a first live run, request a very small GFS area and one or two time
   steps, select **Copernicus Marine North-West Shelf** currents, and enter a
   free Copernicus Marine account login. Set **Output GRIB** directly in the
   form, or use **Browse…** to choose it, then press **Generate GRIB**. This
   button starts the supervised job immediately; no second save dialog should
   appear. Verify the resulting GRIB opens automatically and contains current
   vectors.
7. Test a local merge using non-sensitive GRIB files. Choose **Local GRIB
   file…** in the weather and/or current source selector, select each enabled
   file input, choose the output and generate. A local weather file replaces
   online weather and wave downloads (wave records already in that file are
   preserved); a local current file replaces the online current source. The
   helper receives read-only access to only those selected files. Verify that
   the output opens automatically and contains the expected combined fields.
   Repeat with a forecast beyond a primary provider's range and verify the
   declared long-range fallback is used, or the job fails clearly when no
   fallback was selected. A failed job or invalid result must leave the
   previously open valid dataset and overlays unchanged.
8. Start a larger operation and press **Cancel**; the UI must remain usable.
9. Try a small text file renamed to `.grb`; the error must be contained and
   OpenCPN must remain operational.
10. Close and reopen iGRIB, then exit and restart Test-OpenCPN.
11. Verify a normal installed OpenCPN still uses its original profile and
   plugins.

Provider availability and forecast contents depend on upstream services.
Failure messages should be reported as structured provider failures rather
than treated as proof that the runtime itself failed.

Copernicus Marine accounts can be created at the official
[registration page](https://data.marine.copernicus.eu/register). iGRIB keeps a
remembered password in the operating-system credential store, not in its job
file or plugin settings. If no credential store is available, the password is
used for that generation only. North-West Shelf currents cover 20 W to 13 E
and 40 N to 65 N; select the global current model outside that area.

## 6. Exercise portable-plugin interoperability

After opening or generating a multi-time GRIB in iGRIB, select the
**iWeatherRouting** toolbar action:

1. Confirm the forecast summary names the iGRIB file and reports its forecast
   time count. This is service discovery through the host broker; no native
   xGRIB message strings are involved.
2. Confirm the bundled **Nicholson35_Mk1_cruising_realistic.pol** model is
   reported as loaded. Use the vessel-performance picker to select another
   OpenCPN weather-routing `.pol` file, or a boat `.xml` containing up to eight
   `Polar FileName` entries. For XML, referenced polars must be in the same
   directory as the XML or its sibling `polars` directory. Close and reopen
   the panel, then restart Test-OpenCPN and confirm the selected model is
   remembered and revalidated. An invalid/missing model must prevent routing
   with a clear error; it must never fall back to fabricated performance.
3. Choose each supported position source in turn: live vessel position,
   OpenCPN waypoint, latest chart-cursor position and manual coordinates.
   **Refresh OpenCPN positions** must update the waypoint lists after marks are
   created or imported. Choose a short start/destination pair entirely inside
   the GRIB and GSHHS coverage. Keep the first run below roughly 30 NM, use the
   default one-hour step and press **Calculate route**.
4. Verify progress remains responsive and the result reports points, distance,
   duration and states examined. The magenta route should appear on the chart.
   The component requests forecast-time/position batches; iGRIB decodes the
   required frames in its bounded sidecar and keeps only a small LRU cache.
5. Set minimum and maximum true-wind angles (for example 40° and 160°), then
   try a minimum greater than the maximum, which must be rejected before the
   job starts. Exercise the separate true/apparent wind-speed and wave-height
   limits, current/wave coverage policy, efficiency and manoeuvre controls.
   A rejected or exhausted search must produce a structured failure without
   disabling either plugin. Close and reopen the panel, then restart
   Test-OpenCPN and confirm the settings persist across the application
   session.
6. Start a longer calculation and press **Cancel**. Cancellation must return
   control to the UI and OpenCPN must remain operational.
7. Enable shoreline avoidance and try a route which crosses land. The host's
   batched GSHHS screening should reject those candidate segments. This is an
   experimental shoreline screen, not a hydrographic or passage-safety claim.
8. Export a completed result as GPX, inspect it, and confirm it contains route
   points with UTC timestamps. Treat it as advisory output only.
9. On the Advanced page, enable the forward departure window. Verify up to
   four isolated Wasm stores run concurrently, the earliest safe arrival is
   selected and the other successful departures remain as thin comparison
   overlays. Cancel this run and confirm every active store stops.
10. Remove iGRIB from a fresh test profile and confirm routing fails clearly
   with no provider rather than substituting fabricated weather.

The component interpolates actual tabulated boat speed by true wind speed and
angle. For a multi-polar boat XML it currently selects the fastest applicable
table; native Weather Routing crossover contours and sail-change hysteresis
are not yet represented. The Advanced page can compare a bounded forward
departure window in a four-worker component-instance pool and retain
alternatives. Ordered intermediate-waypoint sequences remain a follow-on
interface. Testers should validate their own polar independently and must not
interpret this experimental engine as a commissioned vessel model.

## 7. Conformance tests

Run the package/helper checks without a fixture:

```sh
portable-runtime/beta/run-conformance.sh
```

For real decode and generator coverage, supply a non-sensitive GRIB fixture:

```sh
portable-runtime/beta/run-conformance.sh /path/to/fixture.grb
```

The second form also exercises the versioned generator job protocol using the
fixture as an existing-file input. The source fixture is never modified.

To opt into a small authenticated, live Copernicus current download from the
conformance runner, enter credentials without placing the password on the
command line:

```sh
read -r -p "Copernicus Marine username or email: " \
  COPERNICUSMARINE_SERVICE_USERNAME
read -r -s -p "Copernicus Marine password: " \
  COPERNICUSMARINE_SERVICE_PASSWORD
printf '\n'
export COPERNICUSMARINE_SERVICE_USERNAME
export COPERNICUSMARINE_SERVICE_PASSWORD
portable-runtime/beta/run-conformance.sh --live-copernicus
unset COPERNICUSMARINE_SERVICE_USERNAME COPERNICUSMARINE_SERVICE_PASSWORD
```

This is an explicit network test. It downloads one hour over a small Irish Sea
box, validates the generated GRIB stream and verifies both current-vector
components. The report contains no username or password.

## 8. Reporting a result

Create a privacy-reviewed diagnostic archive:

```sh
python3 portable-runtime/beta/collect_diagnostics.py
```

The collector includes versions, hashes, selected CMake settings, shared
library resolution, portable CTest output and a redacted tail of the isolated
OpenCPN log. It deliberately excludes the OpenCPN configuration, charts,
routes, credentials, GRIB files and plugin-private downloaded data. Inspect
the archive before sharing it.

Use [the beta report template](docs/portable-plugin-runtime/beta-test-report-template.md)
when filing a GitHub issue. Include whether the problem is reproducible after
rebuilding, the exact action, expected/actual result and the diagnostic
archive if appropriate.

## Troubleshooting

### `wasm32-wasip2` or Rust version errors

Run `rustup update stable`, select stable using `rustup default stable`, and
rerun the build. A distribution Rust package older than 1.94 cannot compile
Wasmtime 46.

### Cargo reports an offline dependency is absent

The CMake phase is intentionally offline. Rerun `build-linux.sh` without
`OCPN_BETA_SKIP_FETCH`; it fetches both locked Cargo graphs before configuring.

### CMake cannot find ecCodes or JsonCpp

Check `pkg-config --modversion eccodes jsoncpp`. Install the relevant
development packages, not only runtime libraries.

### iGRIB refuses to execute a helper

This is fail-closed behaviour. On Linux both `/usr/bin/bwrap` and
`/usr/bin/prlimit` must exist. Do not bypass the check or run the packaged
helper manually with broader permissions as a workaround.

### iGRIB is not discovered

Check `build-portable-beta/stage/config/opencpn.log` for `Portable plugin` and
verify the package exists below
`build-portable-beta/stage/config/portable-plugins/org.opencpn.igrib`.

### Reset only the beta profile

Stop Test-OpenCPN, then move `build-portable-beta/stage/config` elsewhere and
rerun `build-linux.sh`. Do not delete or alter `~/.opencpn`; it is not part of
this beta setup.

## Scope of this beta

This is a substantial functional architecture proof with portable equivalents
for the xGRIB viewer/generator workflows; it is not production navigation
software or a claim of pixel-identical wxWidgets rendering. Package-owned
schema/controller/provider policy is rendered by reusable host services rather
than iGRIB dialogs in core. Remaining production gates—including broader
chart-safety semantics, permission-consent UI, catalogue trust/revocation,
platform code-signing/notarisation and long-term security ownership—are tracked in
`docs/portable-plugin-runtime/prototype-status.md`.
