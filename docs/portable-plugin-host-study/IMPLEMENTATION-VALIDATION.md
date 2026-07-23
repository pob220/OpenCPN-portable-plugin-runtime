# Managed runtime-host implementation validation

This record describes the Linux/source-build proof of concept at the
implementation stop point. It does not claim Windows, macOS, Android, Flatpak
or OpenCPN catalogue support.

## Outcome

The Portable Plugin Manager is a conventional OpenCPN plugin loaded by an
otherwise stock OpenCPN 5.14.0 process. It installs, verifies, approves,
enables, disables, unloads, updates and rolls back signed portable packages.
Each enabled package can register its own OpenCPN toolbar action. The three
reference packages are working applications rather than lifecycle stubs:

| Package | Representative workload | Result |
|---|---|---|
| iPolars 0.1.4 | declarative editor and polar diagram, scoped files, NMEA/VDR/CSV/LogbookKonni import, and boat/polar interchange | runtime-tested |
| iGRIB 0.1.0 | 44.9 MB real GRIB, supervised native helper and environmental service | runtime-tested and GUI-tested |
| iWeatherRouting 0.1.0 | cancellable background computation, typed iGRIB consumption, chart checks and route export | runtime-tested and GUI-tested |

The host boundary uses the published `ocpn_plugin.h` interface. The reference
plugin algorithms and resources were retained, but their Wasm components are
rebuilt against the current portable contract and packaged for the Manager;
they are not legacy prototype archives copied into a new directory.

## Reproducible evidence

The following checks were run from the implementation worktree on 23 July
2026:

| Requirement | Evidence | Classification |
|---|---|---|
| Stock OpenCPN unchanged | `tools/verify-stock-core.sh` passes against `Release_5.14.0` | build-and-package-tested |
| Independent native plugin | separate Manager configure/build/install; undefined-symbol and `ldd` inspection; real stock loader log reports API 1.21 compatible | GUI-tested |
| Host subsystems | all 13 Manager CTest executables pass | unit-tested |
| Wasm components | bridge tests pass; iWeatherRouting's 11 Rust tests pass; all three release components build locked/offline | unit-tested |
| Signed archives | deterministic packaging, checksums, Python archive conformance and native Manager inspection pass | build-and-package-tested |
| Store lifecycle | install, exact permission approval, enable/disable/re-enable, unload, update, rollback, tamper rejection and removal exercised | runtime-tested |
| Failure containment | malformed archives, bad signatures, helper failures, Wasm traps, cancellation, shutdown and recovery exercised | runtime-tested |
| iPolars interchange | saved `.pol` and multi-polar boat XML are loaded by the exact iWeatherRouting polar parser | runtime-tested |
| iGRIB data path | 465 messages, 86 forecast times and 5,960 retained samples decoded from the real fixture; complete generator path passes | runtime-tested |
| Weather route | Holyhead–Dún Laoghaire completes through reverse-isocrone recovery and independent chronological replay | runtime-tested |
| Toolbar integration | Manager, iGRIB, iPolars and iWeatherRouting actions registered in a real stock OpenCPN session | GUI-tested |
| Renderer integration | real iGRIB dataset overlay exercised through the dedicated OpenGL callback on 5.14 and the public wxDC/Vulkan presentation path on the separate 5.15 working build | GUI-tested |

`tools/runtime-host/build-linux.sh` rebuilds stock OpenCPN, all reference
packages and the independently owned Manager, runs native tests and installs
the packages through the Manager's transaction code. The package, lifecycle,
idle/startup and workload scripts are documented in
`tools/runtime-host/README.md`.

## Route result

The installed-package probe used:

- real 44,864,836-byte GRIB fixture;
- bundled Nicholson 35 cruising polar;
- 23 July 2026 10:00 UTC departure;
- Holyhead offshore (`53.336985, -4.607470`) to Dún Laoghaire offshore
  (`53.308408, -6.119702`).

Both measured runs completed the same independently validated 44-point,
59.457 NM route in 77,400 simulated seconds. The solver used forward adaptive
isochrones, automatic fine-corridor refinement and reverse-isocrone recovery.
The diagnostic also declares the time-dependent graph fallback in the cascade.
An intentionally more restrictive wind-angle run reached that graph stage and
reported an explicit policy/state-budget failure rather than hanging.

## Renderer result

The Manager contains no Vulkan-specific plugin API and does not link to
renderer-private OpenCPN classes. It declares both public multi-canvas overlay
callbacks:

- stock OpenCPN 5.14 selected Mesa Intel Iris Xe OpenGL 4.6 Compatibility,
  loaded the real fixture, and reported
  `environment-overlay-rendered path=opengl-compatibility dataset=ready`;
- the separate read-only-tested OpenCPN 5.15 working build selected the Intel
  Iris Xe Vulkan device (Vulkan API 1.4, FIFO presentation), loaded the same
  fixture, and reported
  `environment-overlay-rendered path=wxdc dataset=ready
  renderer-compatible=software,vulkan`.

This matches the 5.15 presenter architecture: plugin wxDC overlays are drawn
into the complete software chart frame before Vulkan uploads it. The OpenGL
callback is not invoked in Vulkan mode. Its separate iGRIB compatibility
surface is bounded to 8192 pixels per dimension and 16,777,216 pixels total;
allocation failure skips that frame with a diagnostic rather than terminating
OpenCPN.

## Measured Linux resources

These are workstation measurements, not universal requirements. RSS includes
stock OpenCPN and shared-library pages; process-tree workload RSS additionally
includes supervised helpers. The module was a `RelWithDebInfo` development
build, so its 52,703,008-byte on-disk size includes debug information.

Two settled launches gave:

| Enabled set | Warm startup | Settled RSS |
|---|---:|---:|
| stock OpenCPN | 632 ms | 225,716 KiB |
| Manager only | 632 ms | 230,704 KiB |
| Manager + iGRIB | 736 ms | 240,492 KiB |
| Manager + iGRIB + iWeatherRouting | 1,050 ms | 243,780 KiB |
| Manager + all three packages | 1,261 ms | 245,212 KiB |

On this system the all-package settled increment over the second stock launch
was about 19.0 MiB. Installed package payloads were 29,040,300 bytes for
iGRIB, 309,665 bytes for iWeatherRouting and 238,205 bytes for iPolars.

The final complete real route run took 70.676 seconds with a sampled
process-tree peak of 536,600 KiB, consistent with the preceding pair of
roughly 70–71-second runs. The real iGRIB workbench reached dataset readiness
in 4.323 seconds; it settled at 283,092 KiB and peaked at 356,408 KiB while
helpers were active. No helper remained after shutdown. These are deliberately
demanding workloads and are the relevant figures for deciding whether an older
navigation computer should run iGRIB plus weather routing. Users can continue
to use stock OpenCPN without installing or enabling the Manager.

## Declared limits and next gate

This proves that one opt-in conventional host can support small interactive,
large-data/helper and long-running computational portable plugins with a
consistent lifecycle and normal toolbar presence. It does not prove that every
existing native OpenCPN API is available to portable packages. New portable
capabilities should be added deliberately, versioned and permission-gated.

The next gate is maintainer review of this Linux proof of concept. Only after
that decision should the project start other-platform builds, release signing,
catalogue metadata or OpenCPN Alpha publication.
