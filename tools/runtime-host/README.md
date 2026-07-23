# Isolated Linux development environment

`build-linux.sh` builds unmodified OpenCPN 5.14.0, builds the Portable Plugin
Manager independently, runs its unit tests, and installs both below
`/home/paul/RuntimeHost-OpenCPN` by default. Override that location with
`OCPN_RUNTIME_HOST_ROOT`.

`launch-linux.sh` uses a dedicated OpenCPN configuration directory, XDG roots,
native-plugin search path and portable-package store. It deliberately does not
change `HOME`. This protects the normal OpenCPN profile and the separate
Test-OpenCPN prototype.

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
