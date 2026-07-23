# iPolars

iPolars is the small reference portable-runtime package used to exercise the
Portable Plugin Manager lifecycle and declarative UI. It is also a functional
editor for matrix `.pol` files and OpenCPN Weather Routing boat `.xml` files.

The component receives no filesystem paths. Open and Save As are native host
dialogs which issue opaque, package-scoped grants. Read grants are bounded to
8 MiB; save grants are one-shot and committed through an atomic rename.
