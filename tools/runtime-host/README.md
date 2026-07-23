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
