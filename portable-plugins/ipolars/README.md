# iPolars

iPolars is the small reference portable-runtime package used to exercise the
Portable Plugin Manager lifecycle and declarative UI. It is also a functional
editor for matrix `.pol` files and OpenCPN Weather Routing boat `.xml` files.

It can also build a measured polar from:

- checksum-valid live NMEA 0183 VWT or MWV wind plus VHW speed through water;
- recorded NMEA/VDR logs, including timestamp-prefixed sentence lines; and
- CSV/TSV observations with `TWS,TWA,STW` or `AWS,AWA,STW` columns.

Apparent wind is converted vectorially using STW. Motoring, manoeuvring,
unstable, incomplete and SOG-only CSV observations are rejected. Accepted
samples are accumulated in 5-degree TWA by 2-knot TWS bins.

The component receives no filesystem paths. Open and Save As are native host
dialogs which issue opaque, package-scoped grants. Read grants are bounded to
8 MiB; save grants are one-shot and committed through an atomic rename.
