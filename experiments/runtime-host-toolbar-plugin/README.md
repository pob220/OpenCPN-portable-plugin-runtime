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

An optional plugin-shaped Wasmtime lifecycle probe is enabled by configuring
`RUNTIME_HOST_PROBE_BRIDGE_LIBRARY` and
`RUNTIME_HOST_PROBE_BRIDGE_INCLUDE_DIR`. At runtime,
`OCPN_RUNTIME_HOST_PROBE_COMPONENT` names an iGRIB component. The plugin then
creates the runtime, invokes initialize/enable, records guest actions, overlay
and job callbacks, and invokes disable/destroy before toolbar cleanup. This
option is deliberately off by default and does not make the experiment part of
normal packaging. The guest's job request starts a bounded native worker; host
shutdown cancels and joins it, and logs the join latency. The same run logs
plugin-visible private/installed data locations and route, waypoint and chart
directory enumeration through public API 1.21.

On Linux the test build also contains a harmless helper-supervision probe. It
launches itself through `posix_spawn` in a new process group, creates one child,
then verifies cancellation terminates the complete group. It neither downloads
data nor writes outside the build/test environment. This establishes the
mechanism, not equivalent containment on other operating systems or Flatpak.
