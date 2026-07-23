# Portable Plugin Manager

Portable Plugin Manager is a conventional OpenCPN plugin which hosts
capability-limited, WebAssembly-based portable packages. OpenCPN itself remains
stock: the manager uses only the published `ocpn_plugin.h` boundary.

This implementation branch is intentionally stopped before cross-platform
release packaging or OpenCPN catalogue publication. See
`docs/portable-plugin-host-study/IMPLEMENTATION-WORKSPACE.md` for the frozen
baselines, validation classifications and workspace layout.

## Independent Linux build

```sh
cmake -S plugins/portable_plugin_manager_pi \
  -B /path/to/build \
  -DOPENCPN_SOURCE_DIR=/path/to/stock/OpenCPN \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /path/to/build
ctest --test-dir /path/to/build --output-on-failure
```

The plugin must also be loaded and exercised in a real stock OpenCPN process;
a successful module build alone is not runtime evidence.
