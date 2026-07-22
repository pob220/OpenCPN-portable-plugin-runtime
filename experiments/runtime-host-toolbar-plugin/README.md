# Runtime host toolbar probe

This is an isolated feasibility experiment, not release plugin code. It uses
only `ocpn_plugin.h` public APIs and is excluded from the normal OpenCPN build.

It registers the runtime manager and iGRIB during `Init()`. With
`OCPN_RUNTIME_HOST_PROBE_AUTORUN=1`, a UI-thread timer then adds
iWeatherRouting, rejects a duplicate logical action, changes an independent
checked state, removes and re-registers iGRIB, removes iWeatherRouting on a
simulated component trap, and recovers it. Every transition is logged with the
`RUNTIME_HOST_PROBE` prefix.

Build and run the non-GUI registry test:

```sh
cmake -S experiments/runtime-host-toolbar-plugin \
  -B build/runtime-host-toolbar-probe
cmake --build build/runtime-host-toolbar-probe --parallel
ctest --test-dir build/runtime-host-toolbar-probe --output-on-failure
```

For a GUI run, copy the built module only into an isolated test profile's
plugin search path. Do not install this probe into a normal OpenCPN profile.
