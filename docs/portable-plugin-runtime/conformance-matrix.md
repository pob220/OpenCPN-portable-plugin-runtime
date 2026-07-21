# Portable-runtime conformance matrix

Status date: 2026-07-21. A row is **pass** only when the named executable was
run on that architecture and the signed aggregate package was verified. Source
review, cross-compilation and emulation are not counted as passes.

| Target | Local evidence | Required CI evidence | Current status |
|---|---|---|---|
| Linux x86-64 | complete source build, 12 focused tests, runtime-OFF build, installed GUI/helper run | `linux-source-and-stock-gate`, native package conformance | local pass; CI pending |
| Linux ARM64 / Raspberry Pi 64-bit | none on this machine | native `ubuntu-24.04-arm` build and package execution | pending |
| Windows x86-64 | source/Job Object review only | MSVC/vcpkg helper build and Windows package execution | pending |
| macOS Intel | source/sandbox profile review only | `macos-15-intel` helper build and package execution | pending |
| macOS Apple Silicon | source/sandbox profile review only | `macos-15` helper build and package execution | pending |
| Flatpak x86-64 | target selection and outer-sandbox path tested at unit/source level | helper execution inside `org.freedesktop.Sdk//24.08` | pending |
| Flatpak ARM64 | target selection and outer-sandbox path tested at unit/source level | native ARM helper execution inside the ARM SDK sandbox | pending |

The workflow assembles one `.ocpnp` only after every helper artifact exists.
`assemble_multitarget_package.py` verifies the signed Linux source package,
requires byte-identical non-helper payloads, rejects duplicate/conflicting or
undeclared helpers, requires all seven target directories and signs a fresh
complete checksum inventory. Native jobs run the unchanged conformance runner;
Flatpak jobs independently verify the signature then run malformed-input and
generator-protocol checks inside the real SDK sandbox.

The checked-in signing identity is for development conformance only. A green
matrix demonstrates technical packaging/execution portability, not Apple
notarisation, Windows publisher reputation, production catalogue trust or
navigation fitness.
