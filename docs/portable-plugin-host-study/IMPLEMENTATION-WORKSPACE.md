# Managed runtime-host implementation workspace

This branch implements the portable runtime as a conventional OpenCPN plugin
against an otherwise stock OpenCPN 5.14.0 source tree.

## Immutable references

- Stock core: OpenCPN `Release_5.14.0`,
  `91f3b674366068a6ecd61a5e9aba204bba85f57e`.
- Modified-core behavioural oracle:
  `portable-runtime-reference-2026-07-23`,
  `834b7b2b97c665d80f95ce6fe83550175f712da4`.
- Implementation branch: `managed-runtime-host-plugin`.

The oracle remains runnable as Test-OpenCPN but is not an implementation
dependency. Its packages, fixtures and typed interfaces are extraction inputs.

## Local isolation

The implementation uses three independent locations:

| Purpose | Location |
|---|---|
| source worktree | `/home/paul/src/OpenCPN-runtime-host-plugin` |
| build and installed application | `/home/paul/RuntimeHost-OpenCPN` |
| configuration, package store and XDG state | below `/home/paul/RuntimeHost-OpenCPN` |

The RuntimeHost-OpenCPN launcher must never read or write the normal OpenCPN
profile or the Test-OpenCPN profile. It uses a distinct desktop name, portable
configuration directory, plugin search directory and XDG roots.

## Stock-core invariant

Files already present in `Release_5.14.0` remain byte-for-byte stock, apart
from repository-only `.gitignore`, `.gitmodules` and GitHub workflow entries
needed to build and validate the separately owned plugin/runtime sources. New
runtime-host code lives below `plugins/portable_plugin_manager_pi` and is built
independently. It must:

- include only the public `ocpn_plugin.h` API at the OpenCPN boundary;
- never link to an OpenCPN core target or private symbol;
- load into an OpenCPN executable built before the plugin;
- pass `tools/verify-stock-core.sh` in local and hosted validation.

## Release-engineering track

The xGRIB Alpha work established requirements which apply from the first host
shell rather than only at publication:

1. clean builds and one unambiguous package/metadata pair;
2. separate build, package, helper and real OpenCPN runtime classifications;
3. isolated GUI profiles and production-path smoke hooks;
4. Unicode/space paths, process quoting, file-lock and shutdown tests;
5. explicit native ABI/dependency inspection;
6. target-specific focused diagnosis followed by the complete shared matrix;
7. validation separated from credentialled deployment and manual approval;
8. no supported-platform claim without executed evidence.

The implementation stop point is a complete, source-buildable Linux host
plugin with iPolars, iWeatherRouting and iGRIB validated in the isolated stock
application. Cross-platform release rebuilds and OpenCPN Alpha catalogue
publication are explicitly out of scope until a separate decision.

## Evidence classifications

Results use only:

- `unit-tested`;
- `build-and-package-tested`;
- `runtime-tested`;
- `GUI-tested`;
- `not-run`;
- `blocked`.

No lower classification is described as full support. Resource evidence
records installed size, idle RSS, incremental RSS per enabled package, cold
and warm startup, peak workload RSS and cleanup after repeated unload.
