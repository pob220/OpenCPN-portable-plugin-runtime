# Source ledger

Access date for the initial ledger: 2026-07-22.

| Source | Revision | Role |
|---|---|---|
| Local portable-runtime study base | `f85835669f97e3d913e4b26a531e47ae1f061b00` | Modified-core reference and study branch base |
| OpenCPN 5.14.0 release | `91f3b674366068a6ecd61a5e9aba204bba85f57e` | Stock baseline for core-change inventory and compatibility experiments |
| OpenCPN upstream `master` | `bc0e1ededbb10cd44feffc26b7989b6b990fa6eb` | Current public API and implementation comparison |
| Portable repository remote branch | `d9bd9b8b9999f12473b8a849bebd628a9e13fa73` | Remote-state record; deliberately not used as the study base because it trails local work |
| Environmental GRIB generator | `f7311bb340f6c942080b5ad18061c1a8b6b5f000` | Native-helper and environmental-service implementation |
| OpenCPN developer manual, UI integration | accessed 2026-07-22 | Official description of toolbar, menu and preferences APIs; checked against source |
| OpenCPN managed-plugin installation/path manuals | accessed 2026-07-22 | Current supported-target claims, managed/legacy distinction and read-only installer data layout |
| Wasmtime embedding, platform and C API manuals | accessed 2026-07-22; current docs report 48.0.0 | Official engine embedding, library, platform and component-model constraints; repository bridge remains pinned to 46.0.1 |
| `testplugin_pi` | `dfde2b83c94da31b27bc32ad9743d1aad41b8a6e` | Current template/API usage and messaging example |
| `shipdriver_pi` | `d6d6023d32e94fd9ffe09c243d5afe5eaa811383` | SVG/PNG toolbar fallback, toggle state, threads and messaging |
| `weather_routing_pi` | `22dd1f099ff067f6c36a953664a54180fdf9d955` | Routing/navigation API, overlays, jobs, modeless UI and plugin messaging |
| `radar_pi` | `80c8403ee409ddd57897c591330ac8f9e26c9477` | Multi-canvas GL rendering, network threads and dynamic toolbar artwork |
| `ocpn_draw_pi` | `82b3adb50288c1c24566336df451697a3a77a073` | Two toolbar tools owned by one plugin, cleanup, rendering and messaging |
| OpenCPN plugin catalogue | `a928fbdedf48d510fb6180a3bb7b6ba39c13998f` | Current managed-plugin metadata and target inventory |

## Local platform

- OpenCPN source version: `Release_5.14.0-44-gf85835669`
- Public plugin API in study base: 1.21
- Host: Linux x86-64, kernel 6.18.37-1-lts
- Compiler: GCC 16.1.1
- CMake: 4.3.4
- Python: 3.14.6
- Rust: 1.97.1 (`8bab26f4f`, repository-local stable toolchain)
- Cargo: 1.97.1 (`c980f4866`, repository-local stable toolchain)

## External locations

- OpenCPN developer manual UI integration:
  <https://opencpn-manuals.github.io/main/ocpn-dev-manual/0.1/pm-plugin-api-ui-integration.html>
- OpenCPN API overview:
  <https://opencpn-manuals.github.io/main/ocpn-dev-manual/0.1/pm-plugin-api-overview.html>
- OpenCPN managed plugin installation and supported targets:
  <https://opencpn-manuals.github.io/main/opencpn-plugins/misc/plugin-install.html>
- Official managed plugin installation paths (including read-only plugin data):
  <https://opencpn-manuals.github.io/main/plugin-installer/Installation-paths.html>
- Wasmtime C/C++ embedding API:
  <https://docs.wasmtime.dev/c-api/>
- Wasmtime platform support:
  <https://docs.wasmtime.dev/stability-platform-support.html>
- Wasmtime embedding and stability documentation:
  <https://docs.wasmtime.dev/lang.html>
- Repositories are the canonical GitHub origins recorded in
  `source-ledger.json`. Shallow source snapshots used for the audit are under
  `/tmp/runtime-host-study-sources`; they are evidence caches, not vendored
  project inputs.
