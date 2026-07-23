# iPolars

iPolars is the small reference portable-runtime package used to exercise the
Portable Plugin Manager lifecycle and declarative UI. It is also a functional
editor for matrix `.pol` files and OpenCPN Weather Routing boat `.xml` files.
It edits polar cells and axes, interpolates bounded gaps, and creates, names,
adds, removes and reorders the polar references in boat XML documents.
The editable matrix and a symmetric port/starboard polar diagram are generated
from the same canonical component state.

The importer accepts the slash- and backslash-header matrix variants used by
OpenCPN Weather Routing, polar_pi, qtVlm, MaxSea and CSV-style exports. It also
accepts Expedition's `TWS TWA STW ...` pair format. polar_pi's synthetic
zero-wind and 60-knot boundary columns are recognised and removed rather than
mistaken for measured performance.

It can also build a measured polar from:

- manually entered true- or apparent-wind observations;
- checksum-valid live NMEA 0183 VWT or MWV wind plus VHW speed through water;
- recorded NMEA/VDR logs, including timestamp-prefixed sentence lines; and
- one or several CSV/TSV observations with `TWS,TWA,STW` or `AWS,AWA,STW`
  columns; and
- one or several LogbookKonni tab-separated logbooks.

Apparent wind is converted vectorially using STW. Motoring, manoeuvring,
unstable, incomplete and SOG-only CSV observations are rejected. Accepted
samples are accumulated in 5-degree TWA by 2-knot TWS bins.

Compared with the conventional polar_pi reference, iPolars retains the core
create/load/view/edit/save, manual, NMEA/VDR and multi-logbook workflows while
adding boat-XML editing and direct validation with iWeatherRouting's actual
polar parser. It deliberately does not substitute SOG for STW: current can
make those materially different, so such samples are rejected instead of
silently corrupting the polar.

The component receives no filesystem paths. Open and Save As are native host
dialogs which issue opaque, package-scoped grants. Read grants are bounded to
8 MiB; save grants are one-shot and committed through an atomic rename.
